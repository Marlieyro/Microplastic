//
// Менеджер Wi-Fi: єдиний "власник" драйвера Wi-Fi у системі.
//
// Логіка роботи (скінченний автомат, задача "wifi_mgr"):
//
//   Старт ──► є дані в NVS? ──ні──────────────────────────────► PORTAL
//                  │так                                           ▲  │
//                  ▼                                              │  │ користувач ввів
//             CONNECTING ── 5 невдалих спроб ─────────────────────┘  │ SSID/пароль
//                  │ отримано IP                                     ▼
//                  ▼                                          PORTAL_TESTING
//             CONNECTED ◄──── портал вимкнено ◄── FINISHING ◄── IP отримано,
//               │    ▲                                           дані збережено
//   втрата      │    │ IP отримано                           (помилка -> PORTAL
//   з'єднання   ▼    │                                         + текст помилки)
//             RECONNECTING (нескінченно, пауза 1с..60с)
//
//   PORTAL ── кожні 5 хв, якщо ніхто не підключений до AP ──► PORTAL_RETRY_SAVED
//          (спроба підключитися до збереженої мережі; успіх -> CONNECTED)
//
//   Утримання кнопки скидання 3 с (будь-який стан) -> стерти NVS -> PORTAL
//
// Взаємодія з рештою програми — через події ESP-IDF (default event loop):
//   APP_WIFI_EVENT_CONNECTED     — є IP, портал гарантовано вимкнений
//                                  (порт 80 вільний для HTTP Server застосунку)
//   APP_WIFI_EVENT_DISCONNECTED  — з'єднання з роутером втрачено
//   APP_WIFI_EVENT_PORTAL_STARTED / APP_WIFI_EVENT_PORTAL_STOPPED
//

#ifndef MICROPLASTIC_APPWIFIMANAGER_H
#define MICROPLASTIC_APPWIFIMANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "appWiFiCredentials.h"

ESP_EVENT_DECLARE_BASE(APP_WIFI_EVENT);

typedef enum {
    APP_WIFI_EVENT_CONNECTED = 0,
    APP_WIFI_EVENT_DISCONNECTED,
    APP_WIFI_EVENT_PORTAL_STARTED,
    APP_WIFI_EVENT_PORTAL_STOPPED,
} app_wifi_event_id_t;

typedef enum {
    WIFI_MGR_STATE_IDLE = 0,            /* менеджер ще не запущений */
    WIFI_MGR_STATE_CONNECTING,          /* підключення зі збереженими даними після старту */
    WIFI_MGR_STATE_CONNECTED,           /* підключено, є IP */
    WIFI_MGR_STATE_RECONNECTING,        /* з'єднання втрачено, нескінченні повтори */
    WIFI_MGR_STATE_PORTAL,              /* портал працює, очікування користувача */
    WIFI_MGR_STATE_PORTAL_TESTING,      /* перевірка даних, введених на порталі */
    WIFI_MGR_STATE_PORTAL_FINISHING,    /* успіх, портал ще живе кілька секунд */
    WIFI_MGR_STATE_PORTAL_RETRY_SAVED,  /* фонова спроба підключитися до збереженої мережі */
} wifi_mgr_state_t;

/* ---------------- Загальний API для решти програми ---------------- */

/**
 * @brief Неблокуючий старт підсистеми Wi-Fi.
 *        Перед викликом мають бути виконані nvs_flash_init(), esp_netif_init()
 *        та esp_event_loop_create_default() (див. main.c).
 *        Обробники APP_WIFI_EVENT бажано зареєструвати ДО цього виклику.
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Блокує викликаючу задачу до підключення або таймауту.
 * @return true — підключено (є IP).
 */
bool wifi_manager_wait_connected(TickType_t timeout);

/** @brief Чи є зараз з'єднання з роутером (з IP-адресою). */
bool wifi_manager_is_connected(void);

/** @brief Поточний стан автомата (для режимів роботи/діагностики). */
wifi_mgr_state_t wifi_manager_get_state(void);

/** @brief Примусово запустити Captive Portal (збережені дані не стираються). */
esp_err_t wifi_manager_start_portal(void);

/** @brief Стерти збережені дані Wi-Fi і запустити Captive Portal. */
esp_err_t wifi_manager_reset_credentials(void);

/**
 * @brief Керування енергозбереженням радіомодуля (modem sleep) для майбутніх
 *        режимів роботи. Діє лише в режимі STA (коли портал вимкнений).
 *        WIFI_PS_NONE — без енергозбереження; WIFI_PS_MIN_MODEM — за замовчуванням;
 *        WIFI_PS_MAX_MODEM — максимальна економія.
 */
esp_err_t wifi_manager_set_power_save(wifi_ps_type_t ps);

/* ---------------- API для модуля Captive Portal ---------------- */

typedef enum {
    WIFI_MGR_PORTAL_RESULT_IDLE = 0,    /* дані ще не вводились */
    WIFI_MGR_PORTAL_RESULT_TESTING,     /* йде перевірка підключення */
    WIFI_MGR_PORTAL_RESULT_SUCCESS,     /* підключено, дані збережено */
    WIFI_MGR_PORTAL_RESULT_FAILED,      /* помилка, текст у message */
} wifi_mgr_portal_result_t;

typedef struct {
    wifi_mgr_portal_result_t result;
    char ssid[WIFI_CRED_SSID_MAX_LEN + 1];
    char message[160];
    char ip[16];
} wifi_mgr_portal_status_t;

typedef struct {
    char   ssid[WIFI_CRED_SSID_MAX_LEN + 1];
    int8_t rssi;
    bool   is_open;
} wifi_mgr_scan_entry_t;

/** @brief Передати введені користувачем дані на перевірку (асинхронно). */
esp_err_t wifi_manager_portal_submit(const char *ssid, const char *password);

/** @brief Запросити нове сканування і дочекатися його завершення. */
esp_err_t wifi_manager_portal_request_scan(TickType_t timeout);

/** @brief Скопіювати останні результати сканування. @return кількість записів. */
size_t wifi_manager_portal_get_scan_results(wifi_mgr_scan_entry_t *out, size_t max_entries);

/** @brief Поточний статус перевірки даних для сторінки порталу. */
void wifi_manager_portal_get_status(wifi_mgr_portal_status_t *out);

#endif //MICROPLASTIC_APPWIFIMANAGER_H
