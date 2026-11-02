//
// Єдиний файл конфігурації застосунку: піни, мережа, сховище, задачі.
//

#ifndef MICROPLASTIC_APPCONFIG_H
#define MICROPLASTIC_APPCONFIG_H

#include "driver/gpio.h"

/* ------------------------------------------------------------------------
 * Джерело сирих даних
 * 1 — дані HIL-симулятора (приходять з сервера через POST /api/raw і
 *     зберігаються у FLASH). Це ЄДИНИЙ режим, що використовується в роботі.
 * 0 — фізичний сенсор AS7341 (код реалізовано, але він не викликається,
 *     оскільки фізичного пристрою немає).
 * ------------------------------------------------------------------------ */
#define APP_DATA_SOURCE_SIM             1

/* ------------------------------------------------------------------------
 * Піни (за схемою KiCad, ESP32-S3-DevKitC-1-N8R2)
 * ------------------------------------------------------------------------ */
#define APP_PIN_LED_STATUS              GPIO_NUM_4    /* D2, сигнальний LED */
#define APP_PIN_LED_IR                  GPIO_NUM_5    /* D3, IR 940 нм */
#define APP_PIN_LED_BLUE                GPIO_NUM_6    /* D1, Blue 470 нм */
#define APP_PIN_I2C_SCL                 GPIO_NUM_7
#define APP_PIN_I2C_SDA                 GPIO_NUM_8
#define APP_PIN_AS7341_INT              GPIO_NUM_17

/* ------------------------------------------------------------------------
 * Сервер FastAPI (ESP32 — HTTP-клієнт)
 * Адреса за замовчуванням; під час роботи автоматично замінюється IP-адресою
 * відправника останнього запиту до ESP32 (команда, режим, сирі дані).
 * ------------------------------------------------------------------------ */
#define APP_SERVER_DEFAULT_HOST         "192.168.0.222"
#define APP_SERVER_PORT                 8000          /* порт uvicorn за замовчуванням */
#define APP_HTTP_TIMEOUT_MS             5000
#define APP_HTTP_RESP_MAX               512           /* тіло відповіді сервера (JSON) */
#define APP_RESULT_JSON_MAX             4096          /* JSON результату з base64 сирих даних */

/* ------------------------------------------------------------------------
 * HTTP-сервер ESP32 (приймає команди, режими, сирі дані)
 * ------------------------------------------------------------------------ */
#define APP_HTTPD_STACK                 6144
#define APP_HTTPD_MAX_SOCKETS           4             /* +3 службові; решта — HTTP-клієнту */
#define APP_RAW_TEXT_MAX_LEN            12288         /* текстовий кадр сирих даних, байт */
#define APP_JSON_REQ_MAX                256           /* тіло JSON-запиту */

/* ------------------------------------------------------------------------
 * Сховище у FLASH (розділи з partitions.csv)
 * ------------------------------------------------------------------------ */
#define APP_STORE_RAW_LABEL             "rawstore"
#define APP_STORE_RES_LABEL             "resstore"

/* ------------------------------------------------------------------------
 * Логіка станів
 * ------------------------------------------------------------------------ */
#define APP_ERROR_AUTOCLEAR_MS          20000         /* ERROR -> WAITING автоматично */
#define APP_TIME_VALID_AFTER_S          1700000000LL  /* час вважається синхронізованим */

/* ------------------------------------------------------------------------
 * Задачі FreeRTOS: стек (байт) і пріоритет
 * ------------------------------------------------------------------------ */
#define APP_TASK_CTRL_STACK             4096
#define APP_TASK_CTRL_PRIO              6
#define APP_TASK_ACQ_STACK              4096
#define APP_TASK_ACQ_PRIO               5
#define APP_TASK_DSP_STACK              8192
#define APP_TASK_DSP_PRIO               5
#define APP_TASK_NET_STACK              6144
#define APP_TASK_NET_PRIO               4
#define APP_TASK_LED_STACK              2048
#define APP_TASK_LED_PRIO               1
#define APP_CORE_PROCESSING             1             /* acq + dsp на ядрі 1, Wi-Fi — на ядрі 0 */

#endif //MICROPLASTIC_APPCONFIG_H
