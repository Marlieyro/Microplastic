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
#include "esp_wifi.h"

#include "appConfig.h"
#include "appControl.h"
#include "appAcq.h"
#include "appModes.h"
#include "appFlashStore.h"
#include "appHTTPclient.h"
#include "appWiFiManager.h"

static const char *TAG = "APP_CTRL";

#define CTRL_QUEUE_LEN          16
#define CTRL_REPLY_TIMEOUT_MS   2000

typedef enum {
    EV_COMMAND = 0,     /* arg = app_cmd_t, sync */
    EV_SET_MODE,        /* arg = applicationModes_t, sync */
    EV_WIFI_UP,
    EV_WIFI_DOWN,
    EV_RESULT_DONE,     /* arg = ok */
    EV_UPLOAD_DONE,     /* arg = ok */
    EV_ACQ_FAILED,      /* err */
    EV_FAULT,           /* err */
} ctrl_ev_type_t;

typedef struct {
    ctrl_ev_type_t type;
    int32_t        arg;
    const char    *err;     /* лише статичні рядки-літерали */
    bool           sync;
} ctrl_ev_t;

/* ---- Таблиці назв (розширюються разом з enum у appControl.h) ---- */

static const char *const s_state_names[APP_STATE_COUNT] = {
    [APP_STATE_INIT]      = "init",
    [APP_STATE_WAITING]   = "waiting",
    [APP_STATE_WORKING]   = "working",
    [APP_STATE_UPLOADING] = "uploading",
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
static SemaphoreHandle_t  s_req_mutex = NULL;   /* серіалізація синхронних запитів */
static SemaphoreHandle_t  s_reply_sem = NULL;
static volatile app_reply_t s_reply = APP_REPLY_OK;

static volatile app_state_t s_state = APP_STATE_INIT;
static const char * volatile s_last_err = "none";
static TickType_t         s_err_deadline = 0;
static bool               s_pending_raw_pop = false;   /* поточний цикл узяв запис з rawstore */
static applicationModes_t s_mode_before_upload = NORMAL_MODE;

/* ======================================================================
 *                           Допоміжні функції
 * ====================================================================== */

static void set_state(app_state_t st)
{
    if (st == s_state) {
        return;
    }
    ESP_LOGI(TAG, "Стан: %s -> %s", s_state_names[s_state], s_state_names[st]);
    s_state = st;
    app_net_request(APP_NET_REQ_EVENT);      /* повідомити сервер про зміну стану */
}

static void enter_error(const char *err)
{
    s_last_err = (err != NULL) ? err : "unknown";
    ESP_LOGE(TAG, "ПОМИЛКА: %s (автоочищення через %d с)", s_last_err, APP_ERROR_AUTOCLEAR_MS / 1000);
    app_acq_abort();
    s_pending_raw_pop = false;
    s_err_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(APP_ERROR_AUTOCLEAR_MS);
    if (s_state == APP_STATE_ERROR) {
        app_net_request(APP_NET_REQ_EVENT);  /* новий текст помилки */
    }
    set_state(APP_STATE_ERROR);
}

static void leave_error(void)
{
    s_last_err = "none";
    set_state(APP_STATE_WAITING);
}

/* Modem sleep: LOW_ENERGY -> максимальна економія, інші режими -> мінімальна (за замовч.) */
static void apply_power_save(void)
{
    if (!wifi_manager_is_connected()) {
        return;
    }
    const wifi_ps_type_t ps = (getApplicationMode() == LOW_ENERGY_MODE) ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    const esp_err_t err = wifi_manager_set_power_save(ps);
    ESP_LOGI(TAG, "Modem sleep: %s (%s)", ps == WIFI_PS_MAX_MODEM ? "MAX_MODEM" : "MIN_MODEM", esp_err_to_name(err));
}

/* ======================================================================
 *                           Обробка подій
 * ====================================================================== */

static app_reply_t handle_command(app_cmd_t cmd)
{
    switch (cmd) {
    case APP_CMD_START_WORK:
        if (s_state != APP_STATE_WAITING) {
            return APP_REPLY_BUSY;
        }
        if (APP_DATA_SOURCE_SIM && app_store_count(&g_store_raw) == 0) {
            return APP_REPLY_NO_DATA;
        }
        s_pending_raw_pop = APP_DATA_SOURCE_SIM;
        app_acq_trigger();
        set_state(APP_STATE_WORKING);
        return APP_REPLY_OK;

    case APP_CMD_STOP_WORK:
        if (s_state != APP_STATE_WORKING) {
            return APP_REPLY_WRONG_STATE;
        }
        app_acq_abort();
        set_state(APP_STATE_WAITING);
        return APP_REPLY_OK;

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
    if (mode >= APP_MODES_COUNT) {
        return APP_REPLY_BAD_ARG;
    }
    if (s_state != APP_STATE_WAITING) {
        return APP_REPLY_BUSY;               /* режим змінюється лише в стані очікування */
    }
    if (mode == getApplicationMode()) {
        return APP_REPLY_OK;
    }

    if (mode == LOAD_OFF_LOCAL_STORAGE_MODE) {
        if (!wifi_manager_is_connected()) {
            return APP_REPLY_NO_NETWORK;
        }
        s_mode_before_upload = getApplicationMode();
        setApplicationMode(mode);
        ESP_LOGI(TAG, "Режим: %s (у FLASH результатів: %u)", appModes_toName(mode),
                 (unsigned)app_store_count(&g_store_res));
        set_state(APP_STATE_UPLOADING);
        app_net_request(APP_NET_REQ_UPLOAD);
        return APP_REPLY_OK;
    }

    setApplicationMode(mode);
    ESP_LOGI(TAG, "Режим: %s", appModes_toName(mode));
    apply_power_save();
    app_net_request(APP_NET_REQ_EVENT);
    return APP_REPLY_OK;
}

static app_reply_t handle_event(const ctrl_ev_t *ev)
{
    switch (ev->type) {
    case EV_COMMAND:
        return handle_command((app_cmd_t)ev->arg);

    case EV_SET_MODE:
        return handle_set_mode((applicationModes_t)ev->arg);

    case EV_WIFI_UP:
        apply_power_save();
        app_net_request(APP_NET_REQ_HELLO);
        if (s_state == APP_STATE_INIT) {
            set_state(APP_STATE_WAITING);
        }
        break;

    case EV_WIFI_DOWN:
        ESP_LOGW(TAG, "Wi-Fi втрачено; результати зберігатимуться у FLASH");
        break;

    case EV_RESULT_DONE:
        /* Сирий запис видаляється лише після того, як результат надіслано або збережено */
        if (s_pending_raw_pop && ev->arg) {
            if (app_store_pop(&g_store_raw) != ESP_OK) {
                enter_error("raw_store");
                break;
            }
            ESP_LOGI(TAG, "Сирий запис видалено з FLASH, залишилось %u", (unsigned)app_store_count(&g_store_raw));
        }
        s_pending_raw_pop = false;
        if (s_state == APP_STATE_WORKING) {
            if (ev->arg) {
                set_state(APP_STATE_WAITING);
            } else {
                enter_error(ev->err);
            }
        }
        break;

    case EV_UPLOAD_DONE:
        if (getApplicationMode() == LOAD_OFF_LOCAL_STORAGE_MODE) {
            setApplicationMode(s_mode_before_upload);
            ESP_LOGI(TAG, "Вивантаження %s, режим повернуто: %s", ev->arg ? "завершено" : "перервано",
                     appModes_toName(s_mode_before_upload));
        }
        if (s_state == APP_STATE_UPLOADING) {
            set_state(APP_STATE_WAITING);
        }
        break;

    case EV_ACQ_FAILED:
        if (s_state == APP_STATE_WORKING) {
            enter_error(ev->err);
        }
        break;

    case EV_FAULT:
        enter_error(ev->err);
        break;
    }
    return APP_REPLY_OK;
}

static void ctrl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Задача керування запущена, стан: %s", s_state_names[s_state]);

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_state == APP_STATE_ERROR) {
            const int32_t left = (int32_t)(s_err_deadline - xTaskGetTickCount());
            wait = (left > 0) ? (TickType_t)left : 0;
        }

        ctrl_ev_t ev;
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            const app_reply_t r = handle_event(&ev);
            if (ev.sync) {
                s_reply = r;
                xSemaphoreGive(s_reply_sem);
            }
        }

        if (s_state == APP_STATE_ERROR && (int32_t)(xTaskGetTickCount() - s_err_deadline) >= 0) {
            ESP_LOGI(TAG, "Автоматичне очищення помилки «%s»", s_last_err);
            leave_error();
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
    post_event(id == APP_WIFI_EVENT_CONNECTED ? EV_WIFI_UP : EV_WIFI_DOWN, 0, NULL);
}

esp_err_t app_ctrl_start(void)
{
    s_queue = xQueueCreate(CTRL_QUEUE_LEN, sizeof(ctrl_ev_t));
    s_req_mutex = xSemaphoreCreateMutex();
    s_reply_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_queue && s_req_mutex && s_reply_sem, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_CONNECTED,
                                                   &wifi_event_handler, NULL), TAG, "register CONNECTED");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_DISCONNECTED,
                                                   &wifi_event_handler, NULL), TAG, "register DISCONNECTED");

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

void app_ctrl_notify_result_done(bool ok, const char *err)
{
    post_event(EV_RESULT_DONE, ok ? 1 : 0, err);
}

void app_ctrl_notify_upload_done(bool ok)
{
    post_event(EV_UPLOAD_DONE, ok ? 1 : 0, NULL);
}

void app_ctrl_notify_acq_failed(const char *err)
{
    post_event(EV_ACQ_FAILED, 0, err);
}

void app_ctrl_notify_fault(const char *err)
{
    post_event(EV_FAULT, 0, err);
}

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
