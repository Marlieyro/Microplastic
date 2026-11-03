//
// Контекстно-залежна мультибуферизація (див. appCtxBuffer.h).
//
// Оцінка контексту (специфічний контекст забруднення кювети не вимірюється):
//   s_net  = 2 (RSSI >= -60 дБм) | 1 (-60..-75) | 0 (< -75)          вага 10
//   s_batt = 2 (>= 50 %) | 1 (20..50 %) | 0 (5..20 %); немає датчика — 2  вага 5
//   N = 10 * s_net + 5 * s_batt  (0..30)
// Жорсткі правила: немає Wi-Fi або батарея < 5 % -> FLASH.
// Рішення: N < 10 -> FLASH; N < 25 -> мережа без сирих даних; інакше — повністю.
//

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"

#include "appConfig.h"
#include "appCtxBuffer.h"
#include "appControl.h"
#include "appFlashStore.h"
#include "appBattery.h"
#include "appHTTPclient.h"
#include "appWiFiManager.h"

_Static_assert(APP_CTX_POOL_N >= 2 && APP_CTX_POOL_N <= APP_CTX_POOL_N_MAX, "APP_CTX_POOL_N поза межами");

static const char *TAG = "CTX_BUF";

static DspResult          s_pool[APP_CTX_POOL_N];
static app_cycle_meta_t   s_meta[APP_CTX_POOL_N];
static app_ctx_decision_t s_decision[APP_CTX_POOL_N];
static int                s_score[APP_CTX_POOL_N];

static QueueHandle_t q_free = NULL;     /* вільні буфери */
static QueueHandle_t q_eval = NULL;     /* DSP -> ctx */
static QueueHandle_t q_net = NULL;      /* ctx -> net */
static QueueHandle_t q_store = NULL;    /* ctx/net -> store */

static int idx_of(const DspResult *p)
{
    return (int)(p - s_pool);
}

const char *app_ctx_decision_name(app_ctx_decision_t d)
{
    switch (d) {
    case APP_CTX_LOCAL:   return "local";
    case APP_CTX_COMPACT: return "compact";
    case APP_CTX_FULL:    return "full";
    default:              return "unknown";
    }
}

/* Завершення обробки буфера: статистика, повідомлення автомату, повернення в пул */
static void finish(DspResult *p, bool ok, const char *route, int64_t route_us, size_t bytes, const char *err)
{
    const int i = idx_of(p);
    app_stats_log_cycle(&s_meta[i], route, route_us, bytes);
    app_ctrl_notify_result_done(ok, err);
    xQueueSend(q_free, &p, 0);
}

static app_ctx_decision_t evaluate(int *score_out, int *rssi_out, float *batt_out, bool *has_batt_out)
{
    int rssi = -127;
    const bool connected = wifi_manager_is_connected();
    if (connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
    }
    float batt = 0.0f;
    const bool has_batt = app_battery_get(&batt, NULL);

    *rssi_out = rssi;
    *batt_out = batt;
    *has_batt_out = has_batt;

    if (!connected || (has_batt && batt < APP_CTX_BATT_CRIT_PCT)) {
        *score_out = 0;
        return APP_CTX_LOCAL;               /* жорстке правило */
    }
    const int s_net = (rssi >= APP_CTX_RSSI_GOOD_DBM) ? 2 : (rssi >= APP_CTX_RSSI_FAIR_DBM) ? 1 : 0;
    const int s_batt = !has_batt ? 2 : (batt >= APP_CTX_BATT_GOOD_PCT) ? 2 : (batt >= APP_CTX_BATT_FAIR_PCT) ? 1 : 0;
    const int n = APP_CTX_W_NET * s_net + APP_CTX_W_BATT * s_batt;
    *score_out = n;
    if (n < APP_CTX_SCORE_LOCAL) {
        return APP_CTX_LOCAL;
    }
    return (n < APP_CTX_SCORE_COMPACT) ? APP_CTX_COMPACT : APP_CTX_FULL;
}

static void ctx_task(void *arg)
{
    (void)arg;
    for (;;) {
        DspResult *p = NULL;
        if (xQueueReceive(q_eval, &p, portMAX_DELAY) != pdTRUE || p == NULL) {
            continue;
        }
        const int i = idx_of(p);
        s_meta[i].t_route_start = app_stats_now();

        int score = 0, rssi = 0;
        float batt = 0.0f;
        bool has_batt = false;
        const int64_t t0 = app_stats_now();
        const app_ctx_decision_t d = evaluate(&score, &rssi, &batt, &has_batt);
        const int64_t eval_us = app_stats_now() - t0;
        s_decision[i] = d;
        s_score[i] = score;

        ESP_LOGI(STAT_TAG, "ctx: RSSI=%d дБм, батарея=%s%.1f%%, N=%d -> %s (оцінка %.3f мс, вільних буферів %u/%u)",
                 rssi, has_batt ? "" : "н/д ", (double)batt, score, app_ctx_decision_name(d),
                 (double)eval_us / 1000.0, (unsigned)uxQueueMessagesWaiting(q_free), (unsigned)APP_CTX_POOL_N);

        if (d == APP_CTX_LOCAL) {
            xQueueSend(q_store, &p, portMAX_DELAY);
        } else {
            xQueueSend(q_net, &p, portMAX_DELAY);
            app_net_request(APP_NET_REQ_CTX);
        }
        app_ctrl_notify_cycle_queued();     /* збір даних звільнено — цикл може повторюватись */
    }
}

static void store_task(void *arg)
{
    (void)arg;
    for (;;) {
        DspResult *p = NULL;
        if (xQueueReceive(q_store, &p, portMAX_DELAY) != pdTRUE || p == NULL) {
            continue;
        }
        const int64_t t0 = app_stats_now();
        const esp_err_t err = app_store_push(&g_store_res, p, sizeof(DspResult));
        const int64_t us = app_stats_now() - t0;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Результат збережено у FLASH (записів: %u)", (unsigned)app_store_count(&g_store_res));
            finish(p, true, "flash", us, sizeof(DspResult), NULL);
        } else {
            finish(p, false, "flash", us, 0, (err == ESP_ERR_NO_MEM) ? "res_store_full" : "res_store");
        }
    }
}

esp_err_t app_ctx_start(void)
{
    q_free = xQueueCreate(APP_CTX_POOL_N, sizeof(DspResult *));
    q_eval = xQueueCreate(APP_CTX_POOL_N, sizeof(DspResult *));
    q_net = xQueueCreate(APP_CTX_POOL_N, sizeof(DspResult *));
    q_store = xQueueCreate(APP_CTX_POOL_N, sizeof(DspResult *));
    ESP_RETURN_ON_FALSE(q_free && q_eval && q_net && q_store, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");

    for (int i = 0; i < APP_CTX_POOL_N; i++) {
        DspResult *p = &s_pool[i];
        xQueueSend(q_free, &p, 0);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(ctx_task, "ctx", APP_TASK_CTX_STACK, NULL, APP_TASK_CTX_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "ctx");
    ESP_RETURN_ON_FALSE(xTaskCreate(store_task, "store", APP_TASK_STORE_STACK, NULL, APP_TASK_STORE_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "store");
    ESP_LOGI(TAG, "Пул мультибуферизації: N = ceil(%d / %d) + 1 = %d буферів x %u Б",
             APP_CTX_T_NET_MAX_MS, APP_CTX_F_CYCLE_MS, APP_CTX_POOL_N, (unsigned)sizeof(DspResult));
    return ESP_OK;
}

uint32_t app_ctx_pool_size(void)
{
    return APP_CTX_POOL_N;
}

uint32_t app_ctx_free_count(void)
{
    return (q_free != NULL) ? (uint32_t)uxQueueMessagesWaiting(q_free) : 0;
}

bool app_ctx_idle(void)
{
    return app_ctx_free_count() == APP_CTX_POOL_N;
}

DspResult *app_ctx_acquire(TickType_t wait)
{
    DspResult *p = NULL;
    if (q_free == NULL || xQueueReceive(q_free, &p, wait) != pdTRUE) {
        return NULL;
    }
    return p;
}

void app_ctx_submit(DspResult *buf, const app_cycle_meta_t *meta)
{
    const int i = idx_of(buf);
    s_meta[i] = *meta;
    xQueueSend(q_eval, &buf, portMAX_DELAY);
}

bool app_ctx_net_take(DspResult **buf, app_ctx_decision_t *decision, int *score)
{
    DspResult *p = NULL;
    if (q_net == NULL || xQueueReceive(q_net, &p, 0) != pdTRUE) {
        return false;
    }
    const int i = idx_of(p);
    *buf = p;
    *decision = s_decision[i];
    *score = s_score[i];
    return true;
}

void app_ctx_net_done(DspResult *buf, bool sent, int64_t route_us, size_t bytes)
{
    if (sent) {
        finish(buf, true, app_ctx_decision_name(s_decision[idx_of(buf)]), route_us, bytes, NULL);
        return;
    }
    ESP_LOGW(TAG, "Мережа не прийняла результат — перенаправлено у FLASH");
    xQueueSend(q_store, &buf, portMAX_DELAY);
}
