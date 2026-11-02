//
// Created by user on 27.10.26.
//
// Мережева задача "net" (див. опис у appHTTPclient.h).
//
// Формат результату (POST /api/result, application/json) — усі поля DspResult;
// сирі матриці 32x10 uint16 передаються бінарно, закодовані Base64
// (little-endian, рядок за рядком, 640 Б -> 856 символів).
//

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_http_client.h"

#include "appConfig.h"
#include "appHTTPclient.h"
#include "appMsgBuffer.h"
#include "appJson.h"
#include "appFlashStore.h"
#include "appControl.h"
#include "appModes.h"
#include "appWiFiManager.h"
#include "dsp_processor_v1.h"

static const char *TAG = "NET";

static esp_http_client_handle_t s_client = NULL;
static TaskHandle_t      s_task = NULL;
static SemaphoreHandle_t s_host_lock = NULL;
static char              s_host[16] = APP_SERVER_DEFAULT_HOST;

/* Статичні буфери задачі net */
static DspResult s_res;
static char      s_json[APP_RESULT_JSON_MAX];
static char      s_resp[APP_HTTP_RESP_MAX + 1];
static int       s_resp_len = 0;

static const char *const s_status_names[] = {
    [DSP_RESULT_UNINITIALIZED]              = "uninitialized",
    [DSP_RESULT_MICROPLASTICS_DETECTED]     = "detected",
    [DSP_RESULT_MICROPLASTICS_NOT_DETECTED] = "not_detected",
    [DSP_RESULT_ERROR_INVALID_INPUT]        = "invalid_input",
    [DSP_RESULT_ERROR_ADC_SATURATION]       = "adc_saturation",
    [DSP_RESULT_ERROR_NUMERIC]              = "numeric_error",
};

static const char *const s_dominant_names[] = {
    [DSP_DOMINANT_UNDETERMINED] = "undetermined",
    [DSP_DOMINANT_NONPOLAR]     = "nonpolar",
    [DSP_DOMINANT_POLAR]        = "polar",
};

/* ======================================================================
 *                         Формування JSON
 * ====================================================================== */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} json_writer_t;

static void jw_printf(json_writer_t *w, const char *fmt, ...)
{
    if (!w->ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->ok = false;
        return;
    }
    w->len += (size_t)n;
}

/* Нескінченність/NaN не є коректним JSON — замінюються на null */
static void jw_float(json_writer_t *w, const char *key, float v)
{
    if (isfinite(v)) {
        jw_printf(w, ",\"%s\":%.7g", key, (double)v);
    } else {
        jw_printf(w, ",\"%s\":null", key);
    }
}

static void jw_b64(json_writer_t *w, const char *key, const uint8_t *data, size_t len)
{
    jw_printf(w, ",\"%s\":\"", key);
    if (!w->ok) {
        return;
    }
    const size_t n = app_b64_encode(data, len, w->buf + w->len, w->cap - w->len);
    if (n == 0) {
        w->ok = false;
        return;
    }
    w->len += n;
    jw_printf(w, "\"");
}

static int build_result_json(const DspResult *r, bool from_flash)
{
    json_writer_t w = { .buf = s_json, .cap = sizeof(s_json), .len = 0, .ok = true };

    const unsigned st = (unsigned)r->status;
    const unsigned dom = (unsigned)r->dominant_fraction;
    const size_t n_st = sizeof(s_status_names) / sizeof(s_status_names[0]);
    const size_t n_dom = sizeof(s_dominant_names) / sizeof(s_dominant_names[0]);

    jw_printf(&w, "{\"from_flash\":%s", from_flash ? "true" : "false");
    jw_printf(&w, ",\"status\":\"%s\"", st < n_st ? s_status_names[st] : "unknown");
    jw_printf(&w, ",\"flags\":%lu", (unsigned long)r->flags);
    jw_float(&w, "concentration_total_mg_l", r->concentration_total_mg_l);
    jw_float(&w, "concentration_nonpolar_mg_l", r->concentration_nonpolar_mg_l);
    jw_float(&w, "concentration_polar_mg_l", r->concentration_polar_mg_l);
    jw_float(&w, "percent_nonpolar", r->percent_nonpolar);
    jw_float(&w, "percent_polar", r->percent_polar);
    jw_printf(&w, ",\"dominant_fraction\":\"%s\"", dom < n_dom ? s_dominant_names[dom] : "unknown");
    jw_float(&w, "polarity_index", r->polarity_index);
    jw_float(&w, "estimated_turbidity_ntu", r->estimated_turbidity_ntu);
    jw_float(&w, "turbidity_signal_counts", r->turbidity_signal_counts);
    jw_float(&w, "ife_multiplier", r->ife_multiplier);
    jw_float(&w, "lod_nonpolar_mg_l", r->lod_nonpolar_mg_l);
    jw_float(&w, "lod_polar_mg_l", r->lod_polar_mg_l);
    jw_printf(&w, ",\"timestamp\":%llu", (unsigned long long)r->timestamp);
    jw_printf(&w, ",\"timestamp_valid\":%s", r->timestamp_valid ? "true" : "false");
    jw_float(&w, "temp_dark", r->raw.temp_dark);
    jw_float(&w, "temp_ir", r->raw.temp_ir);
    jw_float(&w, "temp_blue", r->raw.temp_blue);

    /* Адреси матриць через offsetof — без вказівників на поля packed-структури */
    const uint8_t *base = (const uint8_t *)r + offsetof(DspResult, raw);
    const size_t   mlen = sizeof(uint16_t) * ROWS * COLS;
    jw_b64(&w, "dark", base + offsetof(SensorData, dark_buf), mlen);
    jw_b64(&w, "ir",   base + offsetof(SensorData, ir_buf),   mlen);
    jw_b64(&w, "blue", base + offsetof(SensorData, blue_buf), mlen);
    jw_printf(&w, "}");

    return w.ok ? (int)w.len : -1;
}

/* ======================================================================
 *                         HTTP
 * ====================================================================== */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const int room = APP_HTTP_RESP_MAX - s_resp_len;
        const int n = (evt->data_len < room) ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_resp + s_resp_len, evt->data, (size_t)n);
            s_resp_len += n;
            s_resp[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* POST JSON на сервер. true — сервер відповів 2xx. Відповідь — у s_resp. */
static bool http_post_json(const char *path, const char *body, int len)
{
    if (!wifi_manager_is_connected()) {
        return false;
    }
    char host[sizeof(s_host)];
    app_net_get_server_host(host, sizeof(host));
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, APP_SERVER_PORT, path);

    s_resp_len = 0;
    s_resp[0] = '\0';
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_client, body, len);

    const esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s: %s", url, esp_err_to_name(err));
        esp_http_client_close(s_client);
        return false;
    }
    const int status = esp_http_client_get_status_code(s_client);
    if (status < 200 || status > 299) {
        ESP_LOGW(TAG, "POST %s: HTTP %d", url, status);
        return false;
    }
    return true;
}

static bool post_result(const DspResult *r, bool from_flash)
{
    const int len = build_result_json(r, from_flash);
    if (len < 0) {
        ESP_LOGE(TAG, "JSON результату не вмістився в %u Б", (unsigned)sizeof(s_json));
        return false;
    }
    return http_post_json("/api/result", s_json, len);
}

/* ======================================================================
 *                         Дії задачі
 * ====================================================================== */

static void handle_result(void)
{
    const applicationModes_t mode = getApplicationMode();

    if (mode != CONTEXT_BUFFERING_MODE && post_result(&s_res, false)) {
        ESP_LOGI(TAG, "Результат надіслано на сервер");
        app_ctrl_notify_result_done(true, NULL);
        return;
    }

    /* Сервер недоступний або режим контекстної буферизації — у FLASH */
    const esp_err_t err = app_store_push(&g_store_res, &s_res, sizeof(s_res));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Результат збережено у FLASH (записів: %u)", (unsigned)app_store_count(&g_store_res));
        app_ctrl_notify_result_done(true, NULL);
    } else {
        app_ctrl_notify_result_done(false, (err == ESP_ERR_NO_MEM) ? "res_store_full" : "res_store");
    }
}

/* Режим LOAD_OFF_LOCAL_STORAGE: peek -> POST -> pop (видалення лише після 2xx) */
static void do_upload(void)
{
    unsigned sent = 0;
    bool ok = true;

    while (app_store_count(&g_store_res) > 0) {
        size_t len = 0;
        const esp_err_t err = app_store_peek(&g_store_res, &s_res, sizeof(s_res), &len);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK || len != sizeof(DspResult)) {
            ESP_LOGE(TAG, "Некоректний запис у resstore — видалено");
            app_store_pop(&g_store_res);
            continue;
        }
        if (!post_result(&s_res, true)) {
            ok = false;
            break;
        }
        app_store_pop(&g_store_res);
        sent++;
    }
    ESP_LOGI(TAG, "Вивантаження FLASH: надіслано %u, залишилось %u", sent, (unsigned)app_store_count(&g_store_res));
    app_ctrl_notify_upload_done(ok);
}

static void send_hello(void)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) {
        return;
    }
    char body[64];
    const int len = snprintf(body, sizeof(body), "{\"ip\":\"" IPSTR "\"}", IP2STR(&ip_info.ip));
    if (!http_post_json("/api/hello", body, len)) {
        return;
    }
    double t = 0;
    if (app_json_get_number(s_resp, "time", &t) && (long long)t > APP_TIME_VALID_AFTER_S) {
        const struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Сервер зареєстрував пристрій, час синхронізовано: %lld", (long long)t);
    } else {
        ESP_LOGI(TAG, "Сервер зареєстрував пристрій (без синхронізації часу)");
    }
}

static void send_event(void)
{
    char body[128];
    const int len = snprintf(body, sizeof(body), "{\"state\":\"%s\",\"mode\":\"%s\",\"err\":\"%s\"}",
                             app_ctrl_state_name(app_ctrl_get_state()),
                             appModes_toName(getApplicationMode()),
                             app_ctrl_last_error());
    http_post_json("/api/event", body, len);
}

static void net_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Мережева задача запущена");

    for (;;) {
        /* Короткий таймаут, щоб між результатами обробляти запити APP_NET_REQ_* */
        const size_t n = xMessageBufferReceive(getAppMsgBuffer(), &s_res, sizeof(s_res), pdMS_TO_TICKS(100));
        if (n == sizeof(DspResult)) {
            handle_result();
        } else if (n > 0) {
            ESP_LOGE(TAG, "Розмір повідомлення %u Б, очікувалось %u Б", (unsigned)n, (unsigned)sizeof(DspResult));
        }

        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, 0) == pdTRUE) {
            if (bits & APP_NET_REQ_HELLO) {
                send_hello();
            }
            if (bits & APP_NET_REQ_UPLOAD) {
                do_upload();
            }
            if (bits & APP_NET_REQ_EVENT) {
                send_event();
            }
        }
    }
}

/* ======================================================================
 *                         Публічний API
 * ====================================================================== */

esp_err_t app_net_start(void)
{
    s_host_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_host_lock != NULL, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");

    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/api/hello", s_host, APP_SERVER_PORT);
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,      /* лише для заголовків; тіло передається напряму */
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };
    s_client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_FAIL, TAG, "esp_http_client_init");

    ESP_RETURN_ON_FALSE(xTaskCreate(net_task, "net", APP_TASK_NET_STACK, NULL, APP_TASK_NET_PRIO, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

void app_net_request(uint32_t req_bits)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, req_bits, eSetBits);
    }
}

void app_net_set_server_host(const char *ip)
{
    if (ip == NULL || s_host_lock == NULL || strlen(ip) >= sizeof(s_host)) {
        return;
    }
    bool changed = false;
    xSemaphoreTake(s_host_lock, portMAX_DELAY);
    if (strcmp(s_host, ip) != 0) {
        strlcpy(s_host, ip, sizeof(s_host));
        changed = true;
    }
    xSemaphoreGive(s_host_lock);
    if (changed) {
        ESP_LOGI(TAG, "Нова адреса сервера: %s:%d", ip, APP_SERVER_PORT);
        app_net_request(APP_NET_REQ_HELLO);
    }
}

void app_net_get_server_host(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (s_host_lock == NULL) {
        strlcpy(out, s_host, cap);
        return;
    }
    xSemaphoreTake(s_host_lock, portMAX_DELAY);
    strlcpy(out, s_host, cap);
    xSemaphoreGive(s_host_lock);
}
