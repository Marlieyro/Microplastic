//
// Задача керування — основний цикл прошивки (див. опис автомата в appControl.h).
//

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

#include "appConfig.h"
#include "appControl.h"
#include "appAcq.h"
#include "appModes.h"
#include "appFlashStore.h"
#include "appCtxBuffer.h"
#include "appHTTPclient.h"
#include "appLed.h"
#include "appPower.h"
#include "appStats.h"
#include "appWiFiManager.h"

static const char *TAG = "APP_CTRL";

#define CTRL_QUEUE_LEN          20
#define CTRL_REPLY_TIMEOUT_MS   2000

typedef enum {
    EV_COMMAND = 0,     /* arg = app_cmd_t, sync */
    EV_SET_MODE,        /* arg = applicationModes_t, sync */
    EV_OTA_BEGIN,       /* sync */
    EV_OTA_DONE,        /* arg = ok */
    EV_WIFI_UP,
    EV_WIFI_DOWN,
    EV_PORTAL_UP,
    EV_PORTAL_DOWN,
    EV_RESULT_DONE,     /* arg = ok, err */
    EV_CYCLE_QUEUED,
    EV_UPLOAD_PROGRESS,
    EV_UPLOAD_DONE,     /* arg = ok */
    EV_ACQ_FAILED,      /* err */
    EV_ACQ_ABORTED,
    EV_FAULT,           /* err */
    EV_BUTTON,          /* arg = app_btn_t */
    EV_BATTERY,         /* arg = відсоток * 100 */
} ctrl_ev_type_t;

typedef struct {
    ctrl_ev_type_t type;
    int32_t        arg;
    const char    *err;     /* лише статичні рядки-літерали */
    bool           sync;
} ctrl_ev_t;

/* Програмні таймери автомата */
typedef enum {
    CT_ERROR = 0,       /* автоочищення помилки */
    CT_UPLOAD,          /* сторожовий таймер вивантаження */
    CT_OTA,             /* режим ota без прошивки */
    CT_PORTAL,          /* відкладений запуск Captive Portal */
    CT_RESTART,         /* перезапуск після OTA */
    CT_COUNT
} ctrl_timer_t;

/* ---- Таблиці назв ---- */

static const char *const s_state_names[APP_STATE_COUNT] = {
    [APP_STATE_INIT]      = "init",
    [APP_STATE_WAITING]   = "waiting",
    [APP_STATE_WORKING]   = "working",
    [APP_STATE_UPLOADING] = "uploading",
    [APP_STATE_UPDATING]  = "updating",
    [APP_STATE_ERROR]     = "error",
};

static const struct {
    const char *name;
    app_cmd_t   cmd;
} s_cmd_table[] = {
    { "start_work",  APP_CMD_START_WORK  },
    { "stop_work",   APP_CMD_STOP_WORK   },
    { "clear_error", APP_CMD_CLEAR_ERROR },
};

static const char *const s_reply_names[] = {
    [APP_REPLY_OK]          = "ok",
    [APP_REPLY_BUSY]        = "busy",
    [APP_REPLY_WRONG_STATE] = "wrong_state",
    [APP_REPLY_NO_DATA]     = "no_data",
    [APP_REPLY_NO_NETWORK]  = "no_network",
    [APP_REPLY_BAD_ARG]     = "bad_arg",
    [APP_REPLY_TIMEOUT]     = "timeout",
};

/* ---- Стан автомата (змінюється лише задачею ctrl) ---- */

static QueueHandle_t      s_queue = NULL;
static SemaphoreHandle_t  s_req_mutex = NULL;
static SemaphoreHandle_t  s_reply_sem = NULL;
static volatile app_reply_t s_reply = APP_REPLY_OK;

static volatile app_state_t s_state = APP_STATE_INIT;
static const char * volatile s_last_err = "none";

static TickType_t s_tmr_deadline[CT_COUNT];
static bool       s_tmr_armed[CT_COUNT];

static uint32_t           s_raw_inflight = 0;      /* кадри з rawstore в обробці (ще не видалені) */
static bool               s_cycle_ctx = false;     /* поточний цикл іде шляхом контекстної буферизації */
static applicationModes_t s_prev_mode = NORMAL_MODE;
static bool               s_auto_low = false;      /* LOW_ENERGY увімкнено автоматично (батарея) */
static bool               s_lowbatt_override = false;
static float              s_batt_pct = -1.0f;
static bool               s_first_ip = true;

/* ======================================================================
 *                           Допоміжні функції
 * ====================================================================== */

static void timer_arm(ctrl_timer_t t, uint32_t ms)
{
    s_tmr_deadline[t] = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    s_tmr_armed[t] = true;
}

static void timer_disarm(ctrl_timer_t t)
{
    s_tmr_armed[t] = false;
}

static TickType_t timer_next_wait(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t wait = portMAX_DELAY;
    for (int t = 0; t < CT_COUNT; t++) {
        if (!s_tmr_armed[t]) {
            continue;
        }
        const int32_t diff = (int32_t)(s_tmr_deadline[t] - now);
        if (diff <= 0) {
            return 0;
        }
        if ((TickType_t)diff < wait) {
            wait = (TickType_t)diff;
        }
    }
    return wait;
}

static void set_state(app_state_t st)
{
    if (st == s_state) {
        return;
    }
    ESP_LOGI(TAG, "Стан: %s -> %s", s_state_names[s_state], s_state_names[st]);
    s_state = st;
    app_net_request(APP_NET_REQ_EVENT);      /* повідомити сервер про зміну стану */
    app_led_kick();
}

/* Встановлює режим і відповідний профіль живлення */
static void set_mode(applicationModes_t mode)
{
    if (mode == getApplicationMode()) {
        return;
    }
    ESP_LOGI(TAG, "Режим: %s -> %s", appModes_toName(getApplicationMode()), appModes_toName(mode));
    setApplicationMode(mode);
    const bool low = (mode == LOW_ENERGY_MODE);
    if (low != app_power_is_low()) {
        app_power_apply(low);
    }
    app_net_request(APP_NET_REQ_EVENT);
    app_led_kick();
}

static void enter_error(const char *err)
{
    s_last_err = (err != NULL) ? err : "unknown";
    ESP_LOGE(TAG, "ПОМИЛКА: %s (автоочищення через %d с)", s_last_err, APP_ERROR_AUTOCLEAR_MS / 1000);
    app_acq_abort();
    timer_arm(CT_ERROR, APP_ERROR_AUTOCLEAR_MS);
    if (s_state == APP_STATE_ERROR) {
        app_net_request(APP_NET_REQ_EVENT);  /* новий текст помилки */
    }
    set_state(APP_STATE_ERROR);
}

static void leave_error(void)
{
    timer_disarm(CT_ERROR);
    s_last_err = "none";
    set_state(APP_STATE_WAITING);
}

/* Вихід зі службового режиму (load_off / ota / configuration) у попередній */
static void restore_prev_mode(void)
{
    set_mode(s_prev_mode);
}

/* Спільний старт циклу для команди сервера і кнопки */
static app_reply_t start_cycle(void)
{
    const applicationModes_t mode = getApplicationMode();
    if (s_state != APP_STATE_WAITING || mode == CONFIGURATION_MODE || mode == OTA_MODE ||
        mode == LOAD_OFF_LOCAL_STORAGE_MODE) {
        return APP_REPLY_BUSY;
    }
    const bool ctx = (mode == CONTEXT_BUFFERING_MODE);
    if (ctx && app_ctx_free_count() == 0) {
        return APP_REPLY_BUSY;               /* усі буфери пулу зайняті */
    }
    uint32_t raw_index = 0;
    if (APP_DATA_SOURCE_SIM) {
        if (app_store_count(&g_store_raw) <= s_raw_inflight) {
            return APP_REPLY_NO_DATA;
        }
        raw_index = s_raw_inflight++;
    }
    s_cycle_ctx = ctx;
    app_acq_trigger(raw_index);
    set_state(APP_STATE_WORKING);
    return APP_REPLY_OK;
}

static void raw_release(bool delivered)
{
    if (!APP_DATA_SOURCE_SIM || s_raw_inflight == 0) {
        return;
    }
    s_raw_inflight--;
    if (!delivered) {
        return;                              /* кадр лишається у FLASH для повторної обробки */
    }
    if (app_store_pop(&g_store_raw) != ESP_OK) {
        enter_error("raw_store");
        return;
    }
    ESP_LOGI(TAG, "Сирий кадр видалено з FLASH, залишилось %u", (unsigned)app_store_count(&g_store_raw));
}

/* ======================================================================
 *                           Обробка подій
 * ====================================================================== */

static app_reply_t handle_command(app_cmd_t cmd)
{
    switch (cmd) {
    case APP_CMD_START_WORK:
        return start_cycle();

    case APP_CMD_STOP_WORK:
        if (s_state == APP_STATE_WORKING) {
            app_acq_abort();
            set_state(APP_STATE_WAITING);
            return APP_REPLY_OK;
        }
        if (s_state == APP_STATE_UPLOADING) {
            app_net_upload_abort();
            timer_disarm(CT_UPLOAD);
            restore_prev_mode();
            set_state(APP_STATE_WAITING);
            return APP_REPLY_OK;
        }
        return APP_REPLY_WRONG_STATE;

    case APP_CMD_CLEAR_ERROR:
        if (s_state == APP_STATE_ERROR) {
            leave_error();
        }
        return APP_REPLY_OK;

    default:
        return APP_REPLY_BAD_ARG;
    }
}

static app_reply_t handle_set_mode(applicationModes_t mode)
{
    const applicationModes_t cur = getApplicationMode();
    if (mode >= APP_MODES_COUNT) {
        return APP_REPLY_BAD_ARG;
    }
    if (mode == cur) {
        return APP_REPLY_OK;
    }
    if (cur == CONFIGURATION_MODE) {
        return APP_REPLY_BUSY;               /* вихід — лише через портал (налаштування Wi-Fi) */
    }

    /* Режим конфігурації: відповісти серверу, потім (через 1 с) запустити Captive Portal */
    if (mode == CONFIGURATION_MODE) {
        if (s_state != APP_STATE_INIT && s_state != APP_STATE_WAITING && s_state != APP_STATE_ERROR) {
            return APP_REPLY_BUSY;
        }
        timer_disarm(CT_OTA);
        s_prev_mode = (cur == OTA_MODE) ? s_prev_mode : cur;
        set_mode(CONFIGURATION_MODE);
        timer_arm(CT_PORTAL, APP_PORTAL_DELAY_MS);
        return APP_REPLY_OK;
    }

    if (s_state != APP_STATE_WAITING) {
        return APP_REPLY_BUSY;               /* інші режими змінюються лише в стані очікування */
    }

    switch (mode) {
    case LOAD_OFF_LOCAL_STORAGE_MODE:
        if (!wifi_manager_is_connected()) {
            return APP_REPLY_NO_NETWORK;
        }
        if (app_store_count(&g_store_res) == 0) {
            return APP_REPLY_NO_DATA;        /* нічого вивантажувати — режим не вмикається */
        }
        timer_disarm(CT_OTA);
        s_prev_mode = (cur == OTA_MODE) ? s_prev_mode : cur;
        set_mode(mode);
        set_state(APP_STATE_UPLOADING);
        timer_arm(CT_UPLOAD, APP_UPLOAD_WATCHDOG_MS);
        app_net_upload_start();
        ESP_LOGI(TAG, "Вивантаження FLASH: %u записів", (unsigned)app_store_count(&g_store_res));
        return APP_REPLY_OK;

    case OTA_MODE:
        if (!app_ctx_idle() || s_raw_inflight != 0) {
            return APP_REPLY_BUSY;           /* у пулі ще є недоставлені результати */
        }
        s_prev_mode = cur;
        set_mode(OTA_MODE);
        timer_arm(CT_OTA, APP_OTA_WAIT_MS);
        ESP_LOGI(TAG, "Очікування прошивки POST /api/ota (%d с)", APP_OTA_WAIT_MS / 1000);
        return APP_REPLY_OK;

    default:
        timer_disarm(CT_OTA);
        /* Ручна зміна режиму скасовує автоматичний LOW_ENERGY; поки батарея низька —
         * автоматика повторно його не вмикає (вихід з режиму гарантовано). */
        s_auto_low = false;
        s_lowbatt_override = (s_batt_pct >= 0.0f && s_batt_pct < APP_BATT_LOW_PCT && mode != LOW_ENERGY_MODE);
        set_mode(mode);
        return APP_REPLY_OK;
    }
}

static void handle_button(app_btn_t kind)
{
    const applicationModes_t mode = getApplicationMode();

    if (kind == APP_BTN_SINGLE) {
        if (s_state == APP_STATE_ERROR) {
            leave_error();
            return;
        }
        const app_reply_t r = start_cycle();
        if (r != APP_REPLY_OK) {
            ESP_LOGW(TAG, "Кнопка: старт відхилено (%s)", s_reply_names[r]);
            app_led_signal(APP_LED_SIGNAL_REJECT);
        }
        return;
    }

    /* Подвійне натискання — скидання Wi-Fi (не під час вимірювання/вивантаження/оновлення) */
    const bool allowed = (s_state == APP_STATE_INIT || s_state == APP_STATE_WAITING || s_state == APP_STATE_ERROR) &&
                         mode != CONFIGURATION_MODE && mode != OTA_MODE;
    if (!allowed) {
        ESP_LOGW(TAG, "Кнопка: скидання Wi-Fi відхилено (стан %s, режим %s)",
                 s_state_names[s_state], appModes_toName(mode));
        app_led_signal(APP_LED_SIGNAL_REJECT);
        return;
    }
    ESP_LOGW(TAG, "Кнопка: скидання даних Wi-Fi і запуск Captive Portal");
    wifi_manager_reset_credentials();
}

static void handle_battery(float pct)
{
    s_batt_pct = pct;
    if (pct >= APP_BATT_RECOVER_PCT) {
        s_lowbatt_override = false;
        if (s_auto_low && getApplicationMode() == LOW_ENERGY_MODE && s_state == APP_STATE_WAITING) {
            s_auto_low = false;
            ESP_LOGI(TAG, "Батарея %.1f %% — повернення в normal", (double)pct);
            set_mode(NORMAL_MODE);
        }
    } else if (pct < APP_BATT_LOW_PCT && !s_auto_low && !s_lowbatt_override &&
               getApplicationMode() == NORMAL_MODE && s_state == APP_STATE_WAITING) {
        s_auto_low = true;
        ESP_LOGW(TAG, "Батарея %.1f %% — автоматичний перехід у low_energy", (double)pct);
        set_mode(LOW_ENERGY_MODE);
    }
}

static app_reply_t handle_event(const ctrl_ev_t *ev)
{
    switch (ev->type) {
    case EV_COMMAND:
        return handle_command((app_cmd_t)ev->arg);

    case EV_SET_MODE:
        return handle_set_mode((applicationModes_t)ev->arg);

    case EV_OTA_BEGIN:
        if (getApplicationMode() != OTA_MODE || s_state != APP_STATE_WAITING) {
            return APP_REPLY_WRONG_STATE;
        }
        timer_disarm(CT_OTA);
        set_state(APP_STATE_UPDATING);
        return APP_REPLY_OK;

    case EV_OTA_DONE:
        if (ev->arg) {
            ESP_LOGW(TAG, "Прошивку прийнято — перезапуск через %d мс", APP_RESTART_DELAY_MS);
            timer_arm(CT_RESTART, APP_RESTART_DELAY_MS);
        } else {
            restore_prev_mode();
            enter_error("ota_failed");
        }
        break;

    case EV_WIFI_UP:
        app_power_apply(app_power_is_low());  /* modem sleep діє лише в STA — застосувати після підключення */
        app_net_request(APP_NET_REQ_HELLO);
        if (s_first_ip) {
            s_first_ip = false;
            app_stats_log_boot("Wi-Fi підключено, отримано IP");
        }
        if (s_state == APP_STATE_INIT) {
            set_state(APP_STATE_WAITING);
        }
        app_led_kick();
        break;

    case EV_WIFI_DOWN:
        ESP_LOGW(TAG, "Wi-Fi втрачено; результати зберігатимуться у FLASH");
        app_led_kick();
        break;

    case EV_PORTAL_UP:
        if (getApplicationMode() != CONFIGURATION_MODE) {
            s_prev_mode = getApplicationMode();
            set_mode(CONFIGURATION_MODE);
        }
        break;

    case EV_PORTAL_DOWN:
        if (getApplicationMode() == CONFIGURATION_MODE) {
            restore_prev_mode();
        }
        break;

    case EV_RESULT_DONE:
        raw_release(ev->arg != 0);
        if (!ev->arg) {
            if (s_state != APP_STATE_UPDATING) {
                enter_error(ev->err);
            }
        } else if (s_state == APP_STATE_WORKING && !s_cycle_ctx) {
            set_state(APP_STATE_WAITING);
        }
        break;

    case EV_CYCLE_QUEUED:
        if (s_state == APP_STATE_WORKING && s_cycle_ctx) {
            set_state(APP_STATE_WAITING);
        }
        break;

    case EV_UPLOAD_PROGRESS:
        if (s_state == APP_STATE_UPLOADING) {
            timer_arm(CT_UPLOAD, APP_UPLOAD_WATCHDOG_MS);
        }
        break;

    case EV_UPLOAD_DONE:
        if (s_state == APP_STATE_UPLOADING) {
            timer_disarm(CT_UPLOAD);
            restore_prev_mode();
            set_state(APP_STATE_WAITING);
            ESP_LOGI(TAG, "Вивантаження %s", ev->arg ? "завершено" : "перервано (сервер недоступний)");
        }
        break;

    case EV_ACQ_FAILED:
        raw_release(false);
        if (s_state == APP_STATE_WORKING) {
            enter_error(ev->err);
        }
        break;

    case EV_ACQ_ABORTED:
        raw_release(false);
        break;

    case EV_FAULT:
        enter_error(ev->err);
        break;

    case EV_BUTTON:
        handle_button((app_btn_t)ev->arg);
        break;

    case EV_BATTERY:
        handle_battery((float)ev->arg / 100.0f);
        break;
    }
    return APP_REPLY_OK;
}

static void handle_timer(ctrl_timer_t t)
{
    switch (t) {
    case CT_ERROR:
        if (s_state == APP_STATE_ERROR) {
            ESP_LOGI(TAG, "Автоматичне очищення помилки «%s»", s_last_err);
            leave_error();
        }
        break;
    case CT_UPLOAD:
        if (s_state == APP_STATE_UPLOADING) {
            app_net_upload_abort();
            restore_prev_mode();
            enter_error("upload_timeout");
        }
        break;
    case CT_OTA:
        if (getApplicationMode() == OTA_MODE && s_state == APP_STATE_WAITING) {
            ESP_LOGW(TAG, "Прошивку не отримано за %d с — вихід з режиму ota", APP_OTA_WAIT_MS / 1000);
            restore_prev_mode();
        }
        break;
    case CT_PORTAL:
        if (getApplicationMode() == CONFIGURATION_MODE) {
            ESP_LOGW(TAG, "Запуск Captive Portal (зв'язок із сервером буде втрачено)");
            wifi_manager_start_portal();
        }
        break;
    case CT_RESTART:
        esp_restart();
        break;
    default:
        break;
    }
}

static void ctrl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Задача керування запущена, стан: %s", s_state_names[s_state]);

    for (;;) {
        ctrl_ev_t ev;
        if (xQueueReceive(s_queue, &ev, timer_next_wait()) == pdTRUE) {
            const app_reply_t r = handle_event(&ev);
            if (ev.sync) {
                s_reply = r;
                xSemaphoreGive(s_reply_sem);
            }
        }
        const TickType_t now = xTaskGetTickCount();
        for (int t = 0; t < CT_COUNT; t++) {
            if (s_tmr_armed[t] && (int32_t)(s_tmr_deadline[t] - now) <= 0) {
                s_tmr_armed[t] = false;
                handle_timer((ctrl_timer_t)t);
            }
        }
    }
}

/* ======================================================================
 *                           Публічний API
 * ====================================================================== */

static void post_event(ctrl_ev_type_t type, int32_t arg, const char *err)
{
    if (s_queue == NULL) {
        return;
    }
    const ctrl_ev_t ev = { .type = type, .arg = arg, .err = err, .sync = false };
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Черга подій переповнена, подію %d втрачено", (int)type);
    }
}

static app_reply_t request_sync(ctrl_ev_type_t type, int32_t arg)
{
    if (s_queue == NULL) {
        return APP_REPLY_TIMEOUT;
    }
    app_reply_t r;
    xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    xSemaphoreTake(s_reply_sem, 0);          /* скинути можливу запізнілу відповідь */
    const ctrl_ev_t ev = { .type = type, .arg = arg, .err = NULL, .sync = true };
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        r = APP_REPLY_TIMEOUT;
    } else if (xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(CTRL_REPLY_TIMEOUT_MS)) != pdTRUE) {
        r = APP_REPLY_TIMEOUT;
    } else {
        r = s_reply;
    }
    xSemaphoreGive(s_req_mutex);
    return r;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case APP_WIFI_EVENT_CONNECTED:      post_event(EV_WIFI_UP, 0, NULL); break;
    case APP_WIFI_EVENT_DISCONNECTED:   post_event(EV_WIFI_DOWN, 0, NULL); break;
    case APP_WIFI_EVENT_PORTAL_STARTED: post_event(EV_PORTAL_UP, 0, NULL); break;
    case APP_WIFI_EVENT_PORTAL_STOPPED: post_event(EV_PORTAL_DOWN, 0, NULL); break;
    default: break;
    }
}

esp_err_t app_ctrl_start(void)
{
    s_queue = xQueueCreate(CTRL_QUEUE_LEN, sizeof(ctrl_ev_t));
    s_req_mutex = xSemaphoreCreateMutex();
    s_reply_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_queue && s_req_mutex && s_reply_sem, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(APP_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL), TAG, "register APP_WIFI_EVENT");

    ESP_RETURN_ON_FALSE(xTaskCreate(ctrl_task, "app_ctrl", APP_TASK_CTRL_STACK, NULL,
                                    APP_TASK_CTRL_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

app_reply_t app_ctrl_command(app_cmd_t cmd)
{
    return (cmd < APP_CMD_COUNT) ? request_sync(EV_COMMAND, (int32_t)cmd) : APP_REPLY_BAD_ARG;
}

app_reply_t app_ctrl_set_mode(applicationModes_t mode)
{
    return request_sync(EV_SET_MODE, (int32_t)mode);
}

app_reply_t app_ctrl_ota_begin(void)
{
    return request_sync(EV_OTA_BEGIN, 0);
}

void app_ctrl_notify_result_done(bool ok, const char *err) { post_event(EV_RESULT_DONE, ok ? 1 : 0, err); }
void app_ctrl_notify_cycle_queued(void)                    { post_event(EV_CYCLE_QUEUED, 0, NULL); }
void app_ctrl_notify_upload_progress(void)                 { post_event(EV_UPLOAD_PROGRESS, 0, NULL); }
void app_ctrl_notify_upload_done(bool ok)                  { post_event(EV_UPLOAD_DONE, ok ? 1 : 0, NULL); }
void app_ctrl_notify_acq_failed(const char *err)           { post_event(EV_ACQ_FAILED, 0, err); }
void app_ctrl_notify_acq_aborted(void)                     { post_event(EV_ACQ_ABORTED, 0, NULL); }
void app_ctrl_notify_ota_done(bool ok)                     { post_event(EV_OTA_DONE, ok ? 1 : 0, NULL); }
void app_ctrl_notify_fault(const char *err)                { post_event(EV_FAULT, 0, err); }
void app_ctrl_notify_battery(float percent)                { post_event(EV_BATTERY, (int32_t)(percent * 100.0f), NULL); }
void app_ctrl_button(app_btn_t kind)                       { post_event(EV_BUTTON, (int32_t)kind, NULL); }

app_state_t app_ctrl_get_state(void)
{
    return s_state;
}

const char *app_ctrl_state_name(app_state_t st)
{
    return (st < APP_STATE_COUNT) ? s_state_names[st] : "unknown";
}

const char *app_ctrl_last_error(void)
{
    return s_last_err;
}

const char *app_ctrl_reply_name(app_reply_t r)
{
    return ((size_t)r < sizeof(s_reply_names) / sizeof(s_reply_names[0])) ? s_reply_names[r] : "unknown";
}

bool app_ctrl_cmd_from_name(const char *name, app_cmd_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_cmd_table) / sizeof(s_cmd_table[0]); i++) {
        if (strcmp(name, s_cmd_table[i].name) == 0) {
            *out = s_cmd_table[i].cmd;
            return true;
        }
    }
    return false;
}
