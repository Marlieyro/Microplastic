//
// Статистика роботи системи (див. appStats.h). Виводиться лише в лог із тегом "STAT".
//
// Для повного знімка задач (усіх, включно із системними) і завантаження CPU
// потрібні опції menuconfig:
//   CONFIG_FREERTOS_USE_TRACE_FACILITY=y
//   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
// Без них виводяться купа, IPC, Wi-Fi, батарея, FLASH (без таблиці задач).
//

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_rom_sys.h"

#include "appConfig.h"
#include "appStats.h"
#include "appControl.h"
#include "appModes.h"
#include "appFlashStore.h"
#include "appBattery.h"
#include "appPower.h"
#include "appCtxBuffer.h"
#include "appMsgBuffer.h"
#include "appQueueRawDataBuffer.h"
#include "appWiFiManager.h"

#define META_QUEUE_LEN 6

static QueueHandle_t s_meta_raw = NULL;
static QueueHandle_t s_meta_res = NULL;

static double ms(int64_t us)
{
    return (double)us / 1000.0;
}

esp_err_t app_stats_init(void)
{
    s_meta_raw = xQueueCreate(META_QUEUE_LEN, sizeof(app_cycle_meta_t));
    s_meta_res = xQueueCreate(META_QUEUE_LEN, sizeof(app_cycle_meta_t));
    return (s_meta_raw && s_meta_res) ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_stats_meta_raw_put(const app_cycle_meta_t *m)
{
    if (s_meta_raw != NULL) {
        xQueueSend(s_meta_raw, m, 0);
    }
}

bool app_stats_meta_raw_take(app_cycle_meta_t *m)
{
    /* мітка надсилається одразу ПІСЛЯ кадру — чекаємо її недовго */
    return s_meta_raw != NULL && xQueueReceive(s_meta_raw, m, pdMS_TO_TICKS(50)) == pdTRUE;
}

void app_stats_meta_res_put(const app_cycle_meta_t *m)
{
    if (s_meta_res != NULL) {
        xQueueSend(s_meta_res, m, 0);
    }
}

bool app_stats_meta_res_take(app_cycle_meta_t *m)
{
    return s_meta_res != NULL && xQueueReceive(s_meta_res, m, pdMS_TO_TICKS(50)) == pdTRUE;
}

void app_stats_log_cycle(const app_cycle_meta_t *m, const char *route, int64_t route_us, size_t bytes)
{
    const int64_t now = app_stats_now();
    if (m == NULL || m->t_acq_start == 0) {
        ESP_LOGI(STAT_TAG, "cycle: route=%s T_route=%.3f мс (%u Б), часові мітки циклу недоступні",
                 route, ms(route_us), (unsigned)bytes);
        return;
    }
    const double kbps = (route_us > 0 && bytes > 0) ? ((double)bytes / 1024.0) / ((double)route_us / 1e6) : 0.0;
    ESP_LOGI(STAT_TAG,
             "cycle: T_acq=%.3f T_queue=%.3f T_dsp=%.3f T_ipc=%.3f T_route=%.3f T_total=%.3f мс | "
             "route=%s, %u Б, %.1f КБ/с",
             ms(m->t_acq_end - m->t_acq_start),
             ms(m->t_dsp_start - m->t_acq_end),
             ms(m->t_dsp_end - m->t_dsp_start),
             ms(m->t_route_start - m->t_dsp_end),
             ms(route_us),
             ms(now - m->t_acq_start),
             route, (unsigned)bytes, kbps);
}

void app_stats_log_boot(const char *what)
{
    ESP_LOGI(STAT_TAG, "boot: %s через %.1f мс після старту застосунку", what, ms(app_stats_now()));
}

/* ======================================================================
 *                     Періодичний знімок системи
 * ====================================================================== */

#if (configUSE_TRACE_FACILITY == 1)
#if (configGENERATE_RUN_TIME_STATS == 1)
static TaskHandle_t                 s_prev_handle[APP_STATS_MAX_TASKS];
static configRUN_TIME_COUNTER_TYPE  s_prev_runtime[APP_STATS_MAX_TASKS];
static UBaseType_t                  s_prev_count = 0;
static configRUN_TIME_COUNTER_TYPE  s_prev_total = 0;

static configRUN_TIME_COUNTER_TYPE prev_runtime_of(TaskHandle_t h, bool *found)
{
    for (UBaseType_t i = 0; i < s_prev_count; i++) {
        if (s_prev_handle[i] == h) {
            *found = true;
            return s_prev_runtime[i];
        }
    }
    *found = false;
    return 0;
}
#endif

static void log_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > APP_STATS_MAX_TASKS) {
        n = APP_STATS_MAX_TASKS;
    }
    TaskStatus_t *st = calloc(n, sizeof(TaskStatus_t));
    if (st == NULL) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    n = uxTaskGetSystemState(st, n, &total);

#if (configGENERATE_RUN_TIME_STATS == 1)
    const configRUN_TIME_COUNTER_TYPE d_total = total - s_prev_total;
    ESP_LOGI(STAT_TAG, "tasks: %u (CPU%% — частка одного ядра за період; IDLE0/IDLE1 — простій ядер)", (unsigned)n);
    for (UBaseType_t i = 0; i < n; i++) {
        bool found = false;
        const configRUN_TIME_COUNTER_TYPE prev = prev_runtime_of(st[i].xHandle, &found);
        const double cpu = (found && d_total > 0)
                           ? 100.0 * (double)(st[i].ulRunTimeCounter - prev) / (double)d_total : 0.0;
        ESP_LOGI(STAT_TAG, "  %-16s prio=%2u stack_free_min=%5u Б CPU=%6.2f%%",
                 st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority,
                 (unsigned)st[i].usStackHighWaterMark, cpu);
    }
    s_prev_count = n;
    for (UBaseType_t i = 0; i < n; i++) {
        s_prev_handle[i] = st[i].xHandle;
        s_prev_runtime[i] = st[i].ulRunTimeCounter;
    }
    s_prev_total = total;
#else
    ESP_LOGI(STAT_TAG, "tasks: %u (CPU%% недоступний: увімкніть CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)", (unsigned)n);
    for (UBaseType_t i = 0; i < n; i++) {
        ESP_LOGI(STAT_TAG, "  %-16s prio=%2u stack_free_min=%5u Б",
                 st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority, (unsigned)st[i].usStackHighWaterMark);
    }
#endif
    free(st);
}
#else
static void log_tasks(void)
{
    ESP_LOGI(STAT_TAG, "tasks: таблиця недоступна — увімкніть CONFIG_FREERTOS_USE_TRACE_FACILITY");
}
#endif

static void log_snapshot(void)
{
    ESP_LOGI(STAT_TAG, "==== знімок системи: uptime %.1f с, CPU %u МГц, стан=%s, режим=%s, енергозбереження=%s ====",
             (double)app_stats_now() / 1e6, (unsigned)esp_rom_get_cpu_ticks_per_us(),
             app_ctrl_state_name(app_ctrl_get_state()), appModes_toName(getApplicationMode()),
             app_power_is_low() ? "light sleep + DFS" : "modem sleep MIN");

    ESP_LOGI(STAT_TAG, "heap: вільно %u Б, мінімум за весь час %u Б, найбільший блок %u Б, внутрішня RAM %u Б",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    const unsigned raw_q = getQueueRawDataBuffer() ? (unsigned)uxQueueMessagesWaiting(getQueueRawDataBuffer()) : 0;
    const unsigned mb_free = getAppMsgBuffer() ? (unsigned)xMessageBufferSpacesAvailable(getAppMsgBuffer()) : 0;
    ESP_LOGI(STAT_TAG, "ipc: черга сирих даних %u елем., message buffer вільно %u Б, пул контексту вільно %u/%u",
             raw_q, mb_free, (unsigned)app_ctx_free_count(), (unsigned)app_ctx_pool_size());

    wifi_ap_record_t ap;
    if (wifi_manager_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(STAT_TAG, "wifi: RSSI %d дБм, канал %u", (int)ap.rssi, (unsigned)ap.primary);
    } else {
        ESP_LOGI(STAT_TAG, "wifi: не підключено");
    }

    float pct = 0, mv = 0;
    if (app_battery_get(&pct, &mv)) {
        ESP_LOGI(STAT_TAG, "battery: %.1f %%, %.0f мВ", (double)pct, (double)mv);
    } else {
        ESP_LOGI(STAT_TAG, "battery: датчик недоступний");
    }

    ESP_LOGI(STAT_TAG, "flash: rawstore %u/%u (%.1f%%), resstore %u/%u (%.1f%%)",
             (unsigned)app_store_count(&g_store_raw), (unsigned)g_store_raw.slots,
             g_store_raw.slots ? 100.0 * app_store_count(&g_store_raw) / g_store_raw.slots : 0.0,
             (unsigned)app_store_count(&g_store_res), (unsigned)g_store_res.slots,
             g_store_res.slots ? 100.0 * app_store_count(&g_store_res) / g_store_res.slots : 0.0);

    log_tasks();
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_STATS_PERIOD_MS));
        log_snapshot();
    }
}

esp_err_t app_stats_start(void)
{
    return (xTaskCreate(stats_task, "stats", APP_TASK_STATS_STACK, NULL, APP_TASK_STATS_PRIO, NULL) == pdPASS)
           ? ESP_OK : ESP_ERR_NO_MEM;
}
