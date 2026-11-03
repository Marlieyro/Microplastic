//
// Задача керування (основний цикл прошивки): скінченний автомат станів.
//
//   INIT ──Wi-Fi підключено──► WAITING ◄──────────── цикл завершено ──────┐
//                               │  ▲                                       │
//          start_work / кнопка ─┘  └── stop_work ──── WORKING ─────────────┘
//                               │
//   mode=load_off_local_storage ├────► UPLOADING ── вивантажено / stop_work / 60 с без прогресу ──► WAITING
//   mode=ota + POST /api/ota    └────► UPDATING ── успіх: перезапуск; помилка ──► ERROR
//
//   будь-який стан ── системний збій ──► ERROR ── clear_error / кнопка / 20 с ──► WAITING
//
// Усі зміни стану й режиму виконує ЛИШЕ ця задача. Інші модулі (HTTP-сервер,
// кнопка, мережа, DSP, датчик батареї) надсилають події в її чергу.
//

#ifndef MICROPLASTIC_APPCONTROL_H
#define MICROPLASTIC_APPCONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "appModes.h"

/* Стани системи. Новий стан: додати сюди + назву в s_state_names (appControl.c) */
typedef enum {
    APP_STATE_INIT = 0,
    APP_STATE_WAITING,
    APP_STATE_WORKING,
    APP_STATE_UPLOADING,
    APP_STATE_UPDATING,
    APP_STATE_ERROR,
    APP_STATE_COUNT
} app_state_t;

/* Команди з сервера. Нова команда: додати сюди + рядок у s_cmd_table + case у handle_command() */
typedef enum {
    APP_CMD_START_WORK = 0,
    APP_CMD_STOP_WORK,
    APP_CMD_CLEAR_ERROR,
    APP_CMD_COUNT
} app_cmd_t;

/* Жести кнопки */
typedef enum {
    APP_BTN_SINGLE = 0,
    APP_BTN_DOUBLE,
} app_btn_t;

/* Відповідь автомата на запит HTTP-сервера */
typedef enum {
    APP_REPLY_OK = 0,
    APP_REPLY_BUSY,          /* стан/режим не дозволяє виконати запит */
    APP_REPLY_WRONG_STATE,   /* команда не має сенсу в поточному стані */
    APP_REPLY_NO_DATA,       /* немає даних у FLASH */
    APP_REPLY_NO_NETWORK,    /* немає Wi-Fi */
    APP_REPLY_BAD_ARG,
    APP_REPLY_TIMEOUT,
} app_reply_t;

/** @brief Створює чергу подій і задачу керування; реєструє обробники подій Wi-Fi.
 *         Викликати ДО wifi_manager_start(). */
esp_err_t app_ctrl_start(void);

/* ---- Синхронні запити (HTTP-сервер) ---- */
app_reply_t app_ctrl_command(app_cmd_t cmd);
app_reply_t app_ctrl_set_mode(applicationModes_t mode);
app_reply_t app_ctrl_ota_begin(void);

/* ---- Асинхронні повідомлення від інших задач ---- */
void app_ctrl_notify_result_done(bool ok, const char *err);   /* результат надіслано/збережено */
void app_ctrl_notify_cycle_queued(void);                      /* context: результат передано в пул */
void app_ctrl_notify_upload_progress(void);                   /* net: ще один запис вивантажено */
void app_ctrl_notify_upload_done(bool ok);                    /* net: вивантаження завершено */
void app_ctrl_notify_acq_failed(const char *err);             /* acq: не вдалося отримати кадр */
void app_ctrl_notify_acq_aborted(void);                       /* acq: цикл перервано stop_work */
void app_ctrl_notify_ota_done(bool ok);                       /* httpd: прошивку прийнято/відхилено */
void app_ctrl_notify_fault(const char *err);                  /* будь-який системний збій */
void app_ctrl_notify_battery(float percent);                  /* датчик заряду */
void app_ctrl_button(app_btn_t kind);                         /* кнопка */

/* ---- Читання стану (будь-яка задача) ---- */
app_state_t app_ctrl_get_state(void);
const char *app_ctrl_state_name(app_state_t st);
const char *app_ctrl_last_error(void);
const char *app_ctrl_reply_name(app_reply_t r);
bool app_ctrl_cmd_from_name(const char *name, app_cmd_t *out);

#endif //MICROPLASTIC_APPCONTROL_H
