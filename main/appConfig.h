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
 * Піни (ESP32-S3-DevKitC-1-N8R2)
 * ------------------------------------------------------------------------ */
#define APP_PIN_LED_STATUS              GPIO_NUM_4    /* D2, сигнальний LED */
#define APP_PIN_LED_IR                  GPIO_NUM_5    /* D3, IR 940 нм */
#define APP_PIN_LED_BLUE                GPIO_NUM_6    /* D1, Blue 470 нм */
#define APP_PIN_I2C_SCL                 GPIO_NUM_7    /* спільна шина: AS7341 + MAX17043 */
#define APP_PIN_I2C_SDA                 GPIO_NUM_8
#define APP_PIN_AS7341_INT              GPIO_NUM_17
#define APP_PIN_BUTTON                  GPIO_NUM_1    /* кнопка на GND, внутрішня підтяжка */

/* ------------------------------------------------------------------------
 * Кнопка
 * ------------------------------------------------------------------------ */
#define APP_BTN_POLL_MS                 10            /* період опитування під час натискання */
#define APP_BTN_DEBOUNCE_SAMPLES        3             /* 3 x 10 мс = 30 мс стабільного рівня */
#define APP_BTN_DOUBLE_WINDOW_MS        400           /* вікно другого натискання */
#define APP_BTN_LONG_MS                 3000          /* утримання -> перезапуск */

/* ------------------------------------------------------------------------
 * Датчик заряду MAX17043 (1S Li-ion), I2C-адреса 0x36
 * ------------------------------------------------------------------------ */
#define APP_BATT_I2C_ADDR               0x36
#define APP_BATT_I2C_HZ                 100000
#define APP_BATT_POLL_MS                30000
#define APP_BATT_LOW_PCT                15.0f         /* NORMAL -> LOW_ENERGY автоматично */
#define APP_BATT_RECOVER_PCT            20.0f         /* LOW_ENERGY (авто) -> NORMAL (гістерезис) */

/* ------------------------------------------------------------------------
 * Сервер FastAPI (ESP32 — HTTP-клієнт)
 * Адреса за замовчуванням; під час роботи автоматично замінюється IP-адресою
 * відправника останнього запиту до ESP32.
 * ------------------------------------------------------------------------ */
#define APP_SERVER_DEFAULT_HOST         "192.168.0.222"
#define APP_SERVER_PORT                 8000          /* порт uvicorn за замовчуванням */
#define APP_HTTP_TIMEOUT_MS             5000
#define APP_HTTP_RESP_MAX               512           /* тіло відповіді сервера (JSON) */
#define APP_RESULT_JSON_MAX             4096          /* JSON результату з base64 сирих даних */

/* ------------------------------------------------------------------------
 * HTTP-сервер ESP32
 * ------------------------------------------------------------------------ */
#define APP_HTTPD_STACK                 6144
#define APP_HTTPD_MAX_SOCKETS           4             /* +3 службові; решта — HTTP-клієнту */
#define APP_RAW_TEXT_MAX_LEN            12288         /* текстовий кадр сирих даних, байт */
#define APP_JSON_REQ_MAX                256           /* тіло JSON-запиту */
#define APP_OTA_CHUNK                   4096          /* буфер прийому прошивки */

/* ------------------------------------------------------------------------
 * Сховище у FLASH (розділи з partitions.csv)
 * ------------------------------------------------------------------------ */
#define APP_STORE_RAW_LABEL             "rawstore"
#define APP_STORE_RES_LABEL             "resstore"

/* ------------------------------------------------------------------------
 * Контекстна мультибуферизація (режим context_buffering)
 * Кількість буферів пулу: N = ceil(T / F) + 1, де
 *   T — максимальний час очікування мережі (мс);
 *   F = S / R — час заповнення одного буфера: S — розмір буфера (sizeof(DspResult)),
 *       R — швидкість генерації корисних даних; при одному результаті за цикл
 *       F дорівнює мінімальному періоду циклу вимірювання.
 * ------------------------------------------------------------------------ */
#define APP_CTX_T_NET_MAX_MS            (2 * APP_HTTP_TIMEOUT_MS)
#define APP_CTX_F_CYCLE_MS              2500
#define APP_CTX_POOL_N                  ((APP_CTX_T_NET_MAX_MS + APP_CTX_F_CYCLE_MS - 1) / APP_CTX_F_CYCLE_MS + 1)
#define APP_CTX_POOL_N_MAX              8             /* обмеження SRAM: 8 x 1997 Б */

/* Бали контексту: оцінка 0..2 x вага; поріг рішень */
#define APP_CTX_W_NET                   10
#define APP_CTX_W_BATT                  5
#define APP_CTX_RSSI_GOOD_DBM           (-60)
#define APP_CTX_RSSI_FAIR_DBM           (-75)
#define APP_CTX_BATT_GOOD_PCT           50.0f
#define APP_CTX_BATT_FAIR_PCT           20.0f
#define APP_CTX_BATT_CRIT_PCT           5.0f
#define APP_CTX_SCORE_LOCAL             10            /* N < 10  -> FLASH */
#define APP_CTX_SCORE_COMPACT           25            /* N < 25  -> мережа без сирих даних */

/* ------------------------------------------------------------------------
 * Логіка станів
 * ------------------------------------------------------------------------ */
#define APP_ERROR_AUTOCLEAR_MS          20000         /* ERROR -> WAITING автоматично */
#define APP_UPLOAD_WATCHDOG_MS          60000         /* немає прогресу вивантаження -> вихід */
#define APP_OTA_WAIT_MS                 120000        /* режим ota без прошивки -> вихід */
#define APP_PORTAL_DELAY_MS             1000          /* відповісти серверу перед запуском порталу */
#define APP_RESTART_DELAY_MS            1000          /* після OTA */
#define APP_TIME_VALID_AFTER_S          1700000000LL  /* час вважається синхронізованим */

/* ------------------------------------------------------------------------
 * Енергозбереження (режим low_energy): auto light sleep + DFS + modem sleep MAX
 * Потребує CONFIG_PM_ENABLE і CONFIG_FREERTOS_USE_TICKLESS_IDLE.
 * ------------------------------------------------------------------------ */
#define APP_CPU_MAX_MHZ                 160
#define APP_CPU_MIN_MHZ_LOW             40            /* XTAL */

/* ------------------------------------------------------------------------
 * Статистика
 * ------------------------------------------------------------------------ */
#define APP_STATS_PERIOD_MS             30000
#define APP_STATS_MAX_TASKS             32

/* ------------------------------------------------------------------------
 * Задачі FreeRTOS: стек (байт) і пріоритет
 * ------------------------------------------------------------------------ */
#define APP_TASK_CTRL_STACK             4096
#define APP_TASK_CTRL_PRIO              6
#define APP_TASK_ACQ_STACK              4096
#define APP_TASK_ACQ_PRIO               5
#define APP_TASK_DSP_STACK              8192
#define APP_TASK_DSP_PRIO               5
#define APP_TASK_CTX_STACK              3072
#define APP_TASK_CTX_PRIO               5
#define APP_TASK_STORE_STACK            3072
#define APP_TASK_STORE_PRIO             4
#define APP_TASK_NET_STACK              6144
#define APP_TASK_NET_PRIO               4
#define APP_TASK_BTN_STACK              3072
#define APP_TASK_BTN_PRIO               5
#define APP_TASK_BATT_STACK             3072
#define APP_TASK_BATT_PRIO              2
#define APP_TASK_STATS_STACK            4096
#define APP_TASK_STATS_PRIO             1
#define APP_TASK_LED_STACK              2048
#define APP_TASK_LED_PRIO               1
#define APP_CORE_PROCESSING             1             /* acq + dsp на ядрі 1, Wi-Fi — на ядрі 0 */

#endif //MICROPLASTIC_APPCONFIG_H
