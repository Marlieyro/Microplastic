//
// Created by user on 27.10.26.
//
// HTTP-сервер застосунку (опис маршрутів — у appHTTPserver.h).
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

#include "appConfig.h"
#include "appHTTPserver.h"
#include "appHTTPclient.h"
#include "appControl.h"
#include "appModes.h"
#include "appJson.h"
#include "appFlashStore.h"
#include "appWiFiManager.h"
#include "dsp_processor_v1.h"

#define RECV_TIMEOUT_RETRIES    3
#define RAW_TEXT_NUMBERS        (3 + 3 * ROWS * COLS)   /* 3 температури + 960 відліків */

static const char *TAG = "HTTP_SERVER";

/* ======================================================================
 *                         Допоміжні функції
 * ====================================================================== */

/* Приймає рівно len байтів тіла запиту (обмежена кількість повторів при таймауті) */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int retries = 0;
    while (got < len) {
        const int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= RECV_TIMEOUT_RETRIES) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *err)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", err);
    return send_json(req, status, body);
}

static const char *reply_http_status(app_reply_t r)
{
    switch (r) {
    case APP_REPLY_OK:      return "200 OK";
    case APP_REPLY_BAD_ARG: return "400 Bad Request";
    case APP_REPLY_TIMEOUT: return "503 Service Unavailable";
    default:                return "409 Conflict";
    }
}

/*
 * IP-адреса відправника запиту = адреса сервера FastAPI. ESP32 запам'ятовує її
 * і надалі надсилає результати саме туди (IP ноутбука в прошивці не потрібен).
 * При CONFIG_LWIP_IPV6 сокет сервера двостековий: IPv4-клієнт має адресу ::ffff:a.b.c.d
 */
static void learn_server_ip(httpd_req_t *req)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return;
    }
    char ip[16] = { 0 };
    if (addr.ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
    }
#if CONFIG_LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&addr;
        const uint8_t *b = (const uint8_t *)&a6->sin6_addr;
        static const uint8_t v4_mapped_prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (memcmp(b, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0) {
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        }
    }
#endif
    if (ip[0] != '\0') {
        app_net_set_server_host(ip);
    }
}

/* Приймає невелике JSON-тіло у buf (NUL-термінований) */
static esp_err_t recv_json(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (recv_body(req, buf, req->content_len) != ESP_OK) {
        return ESP_FAIL;
    }
    buf[req->content_len] = '\0';
    return ESP_OK;
}

/*
 * Розбір текстового кадру симулятора (формат форматДанихСирихДляMCU_v2):
 *   3 числа float — температури LED OFF, LED IR, LED ON;
 *   96 рядків по 10 цілих [ a, b, ... ] — 32 DARK, 32 IR, 32 BLUE.
 * Будь-які символи, крім чисел (дужки, коми, пробіли, переноси), — роздільники.
 */
static bool parse_raw_text(const char *s, SensorData *out)
{
    const char *p = s;
    int idx = 0;
    float temps[3] = { 0 };

    while (*p != '\0') {
        if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')) {
            p++;
            continue;
        }
        if (idx >= RAW_TEXT_NUMBERS) {
            return false;                       /* зайві числа */
        }
        char *end = NULL;
        if (idx < 3) {
            temps[idx] = strtof(p, &end);
        } else {
            const long v = strtol(p, &end, 10);
            if (end == p || v < 0 || v > 65535 || *end == '.') {
                return false;                   /* відлік АЦП — ціле 0..65535 */
            }
            const int k = idx - 3;
            const int phase = k / (ROWS * COLS);
            const int row = (k % (ROWS * COLS)) / COLS;
            const int col = k % COLS;
            if (phase == 0) {
                out->dark_buf[row][col] = (uint16_t)v;
            } else if (phase == 1) {
                out->ir_buf[row][col] = (uint16_t)v;
            } else {
                out->blue_buf[row][col] = (uint16_t)v;
            }
        }
        if (end == p) {
            p++;                                /* одиночний '-', '+' або '.' */
            continue;
        }
        p = end;
        idx++;
    }
    if (idx != RAW_TEXT_NUMBERS) {
        return false;
    }
    out->temp_dark = temps[0];
    out->temp_ir = temps[1];
    out->temp_blue = temps[2];
    return true;
}

/* ======================================================================
 *                         Обробники
 * ====================================================================== */

static esp_err_t command_post_handler(httpd_req_t *req)
{
    char body[APP_JSON_REQ_MAX];
    char name[32];
    app_cmd_t cmd;

    learn_server_ip(req);
    if (recv_json(req, body, sizeof(body)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "body");
    }
    if (!app_json_get_str(body, "cmd", name, sizeof(name)) || !app_ctrl_cmd_from_name(name, &cmd)) {
        return send_error(req, "400 Bad Request", "bad_cmd");
    }

    const app_reply_t r = app_ctrl_command(cmd);
    ESP_LOGI(TAG, "Команда «%s»: %s", name, app_ctrl_reply_name(r));
    if (r != APP_REPLY_OK) {
        return send_error(req, reply_http_status(r), app_ctrl_reply_name(r));
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"state\":\"%s\"}", app_ctrl_state_name(app_ctrl_get_state()));
    return send_json(req, "200 OK", resp);
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    char body[APP_JSON_REQ_MAX];
    char name[32];
    applicationModes_t mode;

    learn_server_ip(req);
    if (recv_json(req, body, sizeof(body)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "body");
    }
    if (!app_json_get_str(body, "mode", name, sizeof(name)) || !appModes_fromName(name, &mode)) {
        return send_error(req, "400 Bad Request", "bad_mode");
    }

    const app_reply_t r = app_ctrl_set_mode(mode);
    ESP_LOGI(TAG, "Режим «%s»: %s", name, app_ctrl_reply_name(r));
    if (r != APP_REPLY_OK) {
        return send_error(req, reply_http_status(r), app_ctrl_reply_name(r));
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"mode\":\"%s\"}", appModes_toName(getApplicationMode()));
    return send_json(req, "200 OK", resp);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char resp[160];

    learn_server_ip(req);
    snprintf(resp, sizeof(resp), "{\"state\":\"%s\",\"mode\":\"%s\",\"raw\":%u,\"res\":%u,\"err\":\"%s\"}",
             app_ctrl_state_name(app_ctrl_get_state()),
             appModes_toName(getApplicationMode()),
             (unsigned)app_store_count(&g_store_raw),
             (unsigned)app_store_count(&g_store_res),
             app_ctrl_last_error());
    return send_json(req, "200 OK", resp);
}

static esp_err_t raw_post_handler(httpd_req_t *req)
{
    learn_server_ip(req);

    char ctype[48] = { 0 };
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    const bool binary = (strncmp(ctype, "application/octet-stream", 24) == 0);
    const size_t len = req->content_len;

    /* Стек задачі httpd обмежений — буфери в heap */
    SensorData *frame = calloc(1, sizeof(SensorData));
    if (frame == NULL) {
        return send_error(req, "500 Internal Server Error", "no_mem");
    }

    esp_err_t err = ESP_OK;
    const char *fail = NULL;
    if (binary) {
        if (len != sizeof(SensorData)) {
            fail = "size";
        } else if (recv_body(req, (char *)frame, len) != ESP_OK) {
            err = ESP_FAIL;
        }
    } else {
        if (len == 0 || len > APP_RAW_TEXT_MAX_LEN) {
            fail = "size";
        } else {
            char *text = malloc(len + 1);
            if (text == NULL) {
                fail = "no_mem";
            } else {
                if (recv_body(req, text, len) != ESP_OK) {
                    err = ESP_FAIL;
                } else {
                    text[len] = '\0';
                    if (!parse_raw_text(text, frame)) {
                        fail = "format";
                    }
                }
                free(text);
            }
        }
    }

    if (err != ESP_OK) {                        /* з'єднання обірвано — відповідати нікому */
        free(frame);
        return ESP_FAIL;
    }
    if (fail != NULL) {
        free(frame);
        ESP_LOGW(TAG, "Сирі дані відхилено: %s (%u Б, %s)", fail, (unsigned)len, binary ? "bin" : "text");
        return send_error(req, "400 Bad Request", fail);
    }

    err = app_store_push(&g_store_raw, frame, sizeof(SensorData));
    free(frame);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "full");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "flash");
    }

    const unsigned count = (unsigned)app_store_count(&g_store_raw);
    ESP_LOGI(TAG, "Сирий кадр (%s) збережено у FLASH, у черзі: %u", binary ? "bin" : "text", count);
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"raw\":%u}", count);
    return send_json(req, "200 OK", resp);
}

static const httpd_uri_t s_uri_command = { .uri = "/api/command", .method = HTTP_POST, .handler = command_post_handler };
static const httpd_uri_t s_uri_mode    = { .uri = "/api/mode",    .method = HTTP_POST, .handler = mode_post_handler };
static const httpd_uri_t s_uri_status  = { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler };
static const httpd_uri_t s_uri_raw     = { .uri = "/api/raw",     .method = HTTP_POST, .handler = raw_post_handler };

/* ======================================================================
 *                         Запуск / зупинка
 * ====================================================================== */

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = APP_HTTPD_STACK;
    config.max_open_sockets = APP_HTTPD_MAX_SOCKETS;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Запуск HTTP-сервера на порту %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Помилка запуску HTTP-сервера");
        return NULL;
    }
    httpd_register_uri_handler(server, &s_uri_command);
    httpd_register_uri_handler(server, &s_uri_mode);
    httpd_register_uri_handler(server, &s_uri_status);
    httpd_register_uri_handler(server, &s_uri_raw);
    return server;
}

static void disconnect_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server != NULL) {
        ESP_LOGI(TAG, "Зупинка HTTP-сервера");
        if (httpd_stop(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Не вдалося зупинити HTTP-сервер");
        }
    }
}

static void connect_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL) {
        *server = start_webserver();
    }
}

void app_http_server_init(void)
{
    static httpd_handle_t server = NULL;

    /* Порт 80 у режимі Captive Portal зайнятий порталом, тому сервер
     * стартує/зупиняється за подіями менеджера Wi-Fi. */
    ESP_ERROR_CHECK(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_CONNECTED,
                                               &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_DISCONNECTED,
                                               &disconnect_handler, &server));
}
