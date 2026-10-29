//
// Менеджер Wi-Fi: скінченний автомат підключення до роутера та Captive Portal.
// Опис логіки та станів — у appWiFiManager.h.
//
// Принцип: ТІЛЬКИ задача "wifi_mgr" викликає функції драйвера Wi-Fi
// (connect/disconnect/set_mode/scan). Обробники подій і HTTP-обробники
// лише надсилають повідомлення в її чергу — це виключає гонки.
//

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "driver/gpio.h"

#include "appwifiConfig.h"
#include "appWiFiManager.h"
#include "appWiFiCredentials.h"
#include "appCaptivePortal.h"

ESP_EVENT_DEFINE_BASE(APP_WIFI_EVENT);

static const char *TAG = "WIFI_MGR";

/* Біти групи подій */
#define EVT_CONNECTED_BIT       BIT0    /* STA має IP */
#define EVT_AP_STARTED_BIT      BIT1    /* інтерфейс AP запущений */
#define EVT_SCAN_DONE_BIT       BIT2    /* запит сканування оброблено */

/* Власний код "причини" для таймауту спроби (не є кодом драйвера) */
#define MGR_REASON_TIMEOUT      0

#define PORTAL_START_RETRIES    10
#define PORTAL_START_RETRY_MS   200
#define AP_START_WAIT_MS        3000

/* ---------------- Повідомлення черги ---------------- */

typedef enum {
    MSG_STA_GOT_IP = 0,
    MSG_STA_DISCONNECTED,
    MSG_BUTTON_EDGE,
    MSG_CMD_PORTAL_SUBMIT,
    MSG_CMD_SCAN,
    MSG_CMD_START_PORTAL,
    MSG_CMD_RESET_CREDENTIALS,
} mgr_msg_type_t;

typedef struct {
    mgr_msg_type_t type;
    union {
        uint16_t           reason;  /* MSG_STA_DISCONNECTED */
        esp_ip4_addr_t     ip;      /* MSG_STA_GOT_IP */
        wifi_credentials_t creds;   /* MSG_CMD_PORTAL_SUBMIT */
    } data;
} mgr_msg_t;

/* ---------------- Програмні таймери автомата ---------------- */

typedef enum {
    TMR_CONNECT = 0,        /* наступна спроба esp_wifi_connect() */
    TMR_ATTEMPT_TIMEOUT,    /* спроба триває задовго */
    TMR_PORTAL_SHUTDOWN,    /* вимкнення порталу після успіху */
    TMR_SAVED_RETRY,        /* фонова спроба збереженої мережі */
    TMR_BUTTON_DEBOUNCE,
    TMR_BUTTON_HOLD,
    TMR_COUNT
} mgr_timer_t;

static TickType_t s_tmr_deadline[TMR_COUNT];
static bool       s_tmr_armed[TMR_COUNT];

/* ---------------- Стан ---------------- */

static QueueHandle_t      s_queue = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t  s_data_mutex = NULL;
static esp_netif_t       *s_sta_netif = NULL;
static esp_netif_t       *s_ap_netif = NULL;

static volatile wifi_mgr_state_t s_state = WIFI_MGR_STATE_IDLE;
static bool     s_wifi_started = false;
static bool     s_sta_has_ip = false;
static uint8_t  s_attempt = 0;
static uint8_t  s_max_attempts = 0;
static uint32_t s_backoff_ms = WIFI_MGR_RECONNECT_BACKOFF_MIN_MS;
static char     s_ap_ssid[WIFI_CRED_SSID_MAX_LEN + 1];

static wifi_credentials_t s_pending;            /* дані, що перевіряються з порталу */
static volatile bool      s_btn_msg_pending = false;

/* Дані для сторінки порталу (захищені s_data_mutex) */
static wifi_ap_record_t         s_scan_records[WIFI_MGR_SCAN_MAX_APS];
static wifi_mgr_scan_entry_t    s_scan_cache[WIFI_MGR_SCAN_MAX_APS];
static size_t                   s_scan_count = 0;
static wifi_mgr_portal_status_t s_portal_status;

/* ======================================================================
 *                         Допоміжні функції
 * ====================================================================== */

static void timer_arm(mgr_timer_t t, uint32_t ms)
{
    s_tmr_deadline[t] = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    s_tmr_armed[t] = true;
}

static void timer_disarm(mgr_timer_t t)
{
    s_tmr_armed[t] = false;
}

static bool timer_is_armed(mgr_timer_t t)
{
    return s_tmr_armed[t];
}

/* Скільки тиків можна спати до найближчого таймера */
static TickType_t timer_next_wait(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t wait = portMAX_DELAY;
    for (int t = 0; t < TMR_COUNT; t++) {
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

static bool is_portal_state(wifi_mgr_state_t st)
{
    return st == WIFI_MGR_STATE_PORTAL || st == WIFI_MGR_STATE_PORTAL_TESTING ||
           st == WIFI_MGR_STATE_PORTAL_FINISHING || st == WIFI_MGR_STATE_PORTAL_RETRY_SAVED;
}

static void post_app_event(app_wifi_event_id_t id)
{
    if (esp_event_post(APP_WIFI_EVENT, id, NULL, 0, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "Не вдалося опублікувати подію APP_WIFI_EVENT %d", (int)id);
    }
}

static void portal_status_set(wifi_mgr_portal_result_t result, const char *ssid,
                              const char *message, const char *ip)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_portal_status.result = result;
    strlcpy(s_portal_status.ssid, ssid, sizeof(s_portal_status.ssid));
    strlcpy(s_portal_status.message, message, sizeof(s_portal_status.message));
    strlcpy(s_portal_status.ip, ip, sizeof(s_portal_status.ip));
    xSemaphoreGive(s_data_mutex);
}

static const char *reason_to_text(uint16_t reason)
{
    switch (reason) {
    case MGR_REASON_TIMEOUT:
        return "Роутер не відповів або не видав IP-адресу.";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Невірний пароль.";
    case WIFI_REASON_NO_AP_FOUND:
        return "Мережу не знайдено. Перевірте назву та відстань до роутера.";
    default:
        break;
    }
    /* 210..212: NO_AP_FOUND_W_COMPATIBLE_SECURITY / _IN_AUTHMODE_THRESHOLD / _IN_RSSI_THRESHOLD */
    if (reason >= 210 && reason <= 212) {
        return "Тип захисту мережі не відповідає введеним даним (перевірте пароль).";
    }
    return "Не вдалося підключитися до мережі.";
}

/* ======================================================================
 *                         Робота з драйвером Wi-Fi
 * ====================================================================== */

static void wifi_apply_ap_config(void)
{
    wifi_config_t cfg = {0};
    const size_t len = strlen(s_ap_ssid);
    memcpy(cfg.ap.ssid, s_ap_ssid, len);
    cfg.ap.ssid_len = (uint8_t)len;
    cfg.ap.channel = WIFI_MGR_AP_CHANNEL;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = WIFI_MGR_AP_MAX_CLIENTS;
    cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &cfg));
}

static void wifi_apply_mode(wifi_mode_t mode)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(mode));
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_apply_ap_config();
    }
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }
}

static void sta_attempt_failed(uint16_t reason);

static void sta_attempt_start(void)
{
    if (s_state == WIFI_MGR_STATE_RECONNECTING) {
        ESP_LOGI(TAG, "Спроба перепідключення до роутера");
    } else {
        ESP_LOGI(TAG, "Спроба підключення %u/%u", (unsigned)s_attempt, (unsigned)s_max_attempts);
    }
    timer_arm(TMR_ATTEMPT_TIMEOUT, WIFI_MGR_ATTEMPT_TIMEOUT_MS);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        sta_attempt_failed(MGR_REASON_TIMEOUT);
    }
}

/* Налаштовує STA на вказану мережу і починає серію з max_attempts спроб */
static void sta_sequence_begin(const wifi_credentials_t *creds, uint8_t max_attempts)
{
    timer_disarm(TMR_CONNECT);
    timer_disarm(TMR_ATTEMPT_TIMEOUT);

    /* Перериваємо можливу попередню спробу, інакше set_config поверне помилку.
     * Подія DISCONNECTED з причиною ASSOC_LEAVE від цього виклику ігнорується. */
    esp_wifi_disconnect();

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, creds->ssid, strnlen(creds->ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, creds->password, strnlen(creds->password, sizeof(cfg.sta.password)));
    /* Порожній пароль — відкрита мережа; інакше мінімум WPA2 (WPA3 теж дозволено) */
    cfg.sta.threshold.authmode = (creds->password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    memset(&cfg, 0, sizeof(cfg));

    s_attempt = 1;
    s_max_attempts = max_attempts;
    sta_attempt_start();
}

static void do_scan(void)
{
    const esp_err_t err = esp_wifi_scan_start(NULL, true);   /* блокуюче, ~2-3 с */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Сканування не вдалося: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = WIFI_MGR_SCAN_MAX_APS;
    if (esp_wifi_scan_get_ap_records(&found, s_scan_records) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return;
    }

    wifi_mgr_scan_entry_t list[WIFI_MGR_SCAN_MAX_APS];
    size_t count = 0;

    for (uint16_t i = 0; i < found; i++) {
        const char *ssid = (const char *)s_scan_records[i].ssid;
        if (ssid[0] == '\0') {
            continue;                       /* прихована мережа */
        }
        /* Одна назва — один рядок (кілька точок однієї мережі), лишаємо найсильнішу */
        size_t j = 0;
        for (; j < count; j++) {
            if (strcmp(list[j].ssid, ssid) == 0) {
                break;
            }
        }
        if (j < count) {
            if (s_scan_records[i].rssi > list[j].rssi) {
                list[j].rssi = s_scan_records[i].rssi;
            }
            continue;
        }
        strlcpy(list[count].ssid, ssid, sizeof(list[count].ssid));
        list[count].rssi = s_scan_records[i].rssi;
        list[count].is_open = (s_scan_records[i].authmode == WIFI_AUTH_OPEN);
        count++;
    }

    /* Сортування за рівнем сигналу (спадання) */
    for (size_t i = 1; i < count; i++) {
        const wifi_mgr_scan_entry_t key = list[i];
        size_t j = i;
        while (j > 0 && list[j - 1].rssi < key.rssi) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = key;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    memcpy(s_scan_cache, list, count * sizeof(list[0]));
    s_scan_count = count;
    xSemaphoreGive(s_data_mutex);

    ESP_LOGI(TAG, "Знайдено мереж: %u", (unsigned)count);
}

/* ======================================================================
 *                         Переходи між станами
 * ====================================================================== */

static void schedule_saved_retry(void)
{
    wifi_credentials_t creds;
    if (wifi_credentials_load(&creds) == ESP_OK) {
        timer_arm(TMR_SAVED_RETRY, WIFI_MGR_PORTAL_SAVED_RETRY_MS);
    } else {
        timer_disarm(TMR_SAVED_RETRY);
    }
    memset(&creds, 0, sizeof(creds));
}

static void enter_connected(void)
{
    s_state = WIFI_MGR_STATE_CONNECTED;
    s_backoff_ms = WIFI_MGR_RECONNECT_BACKOFF_MIN_MS;
    timer_disarm(TMR_CONNECT);
    timer_disarm(TMR_ATTEMPT_TIMEOUT);
    timer_disarm(TMR_SAVED_RETRY);
    timer_disarm(TMR_PORTAL_SHUTDOWN);
    xEventGroupSetBits(s_events, EVT_CONNECTED_BIT);
    ESP_LOGI(TAG, "Підключено до Wi-Fi");
    post_app_event(APP_WIFI_EVENT_CONNECTED);
}

static void enter_reconnecting(void)
{
    s_state = WIFI_MGR_STATE_RECONNECTING;
    s_backoff_ms = WIFI_MGR_RECONNECT_BACKOFF_MIN_MS;
    timer_disarm(TMR_ATTEMPT_TIMEOUT);
    timer_arm(TMR_CONNECT, s_backoff_ms);
}

static void portal_start(void)
{
    timer_disarm(TMR_CONNECT);
    timer_disarm(TMR_ATTEMPT_TIMEOUT);
    timer_disarm(TMR_PORTAL_SHUTDOWN);

    s_state = WIFI_MGR_STATE_PORTAL;
    portal_status_set(WIFI_MGR_PORTAL_RESULT_IDLE, "", "", "");

    wifi_apply_mode(WIFI_MODE_APSTA);

    /* DHCP-сервер AP стартує в обробнику AP_START — дочекаємось його */
    if ((xEventGroupWaitBits(s_events, EVT_AP_STARTED_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(AP_START_WAIT_MS)) & EVT_AP_STARTED_BIT) == 0) {
        ESP_LOGW(TAG, "Точка доступу не повідомила про старт вчасно");
    }

    /* Повтори — на випадок, якщо HTTP Server застосунку ще звільняє порт 80 */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < PORTAL_START_RETRIES; i++) {
        err = captive_portal_start(s_ap_netif);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PORTAL_START_RETRY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не вдалося запустити Captive Portal: %s", esp_err_to_name(err));
    }

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    ESP_LOGI(TAG, "Captive Portal активний: мережа «%s», адреса http://" IPSTR "/",
             s_ap_ssid, IP2STR(&ip_info.ip));

    do_scan();
    post_app_event(APP_WIFI_EVENT_PORTAL_STARTED);
    schedule_saved_retry();
}

static void portal_stop(void)
{
    captive_portal_stop();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    timer_disarm(TMR_SAVED_RETRY);
    timer_disarm(TMR_PORTAL_SHUTDOWN);
    portal_status_set(WIFI_MGR_PORTAL_RESULT_IDLE, "", "", "");
    ESP_LOGI(TAG, "Captive Portal вимкнено");
    post_app_event(APP_WIFI_EVENT_PORTAL_STOPPED);
}

/* Перехід у портал з будь-якого НЕпорталового стану */
static void go_to_portal(void)
{
    const bool was_connected = (s_state == WIFI_MGR_STATE_CONNECTED);
    timer_disarm(TMR_CONNECT);
    timer_disarm(TMR_ATTEMPT_TIMEOUT);
    esp_wifi_disconnect();
    s_sta_has_ip = false;
    if (was_connected) {
        xEventGroupClearBits(s_events, EVT_CONNECTED_BIT);
        post_app_event(APP_WIFI_EVENT_DISCONNECTED);   /* HTTP Server застосунку звільнить порт 80 */
    }
    portal_start();
}

/* Серія спроб вичерпана */
static void sta_sequence_failed(uint16_t reason)
{
    switch (s_state) {
    case WIFI_MGR_STATE_CONNECTING:
        ESP_LOGW(TAG, "Не вдалося підключитися після %u спроб — запуск Captive Portal",
                 (unsigned)s_max_attempts);
        portal_start();
        break;

    case WIFI_MGR_STATE_PORTAL_TESTING:
        ESP_LOGW(TAG, "Перевірка мережі «%s» невдала (причина %u)", s_pending.ssid, (unsigned)reason);
        portal_status_set(WIFI_MGR_PORTAL_RESULT_FAILED, s_pending.ssid, reason_to_text(reason), "");
        memset(&s_pending, 0, sizeof(s_pending));
        s_state = WIFI_MGR_STATE_PORTAL;
        schedule_saved_retry();
        break;

    case WIFI_MGR_STATE_PORTAL_RETRY_SAVED:
        ESP_LOGI(TAG, "Збережена мережа досі недоступна");
        s_state = WIFI_MGR_STATE_PORTAL;
        schedule_saved_retry();
        break;

    default:
        break;
    }
}

static void sta_attempt_failed(uint16_t reason)
{
    timer_disarm(TMR_ATTEMPT_TIMEOUT);

    if (s_state == WIFI_MGR_STATE_RECONNECTING) {
        ESP_LOGI(TAG, "Наступна спроба через %" PRIu32 " мс (причина %u)", s_backoff_ms, (unsigned)reason);
        timer_arm(TMR_CONNECT, s_backoff_ms);
        s_backoff_ms = MIN(s_backoff_ms * 2, (uint32_t)WIFI_MGR_RECONNECT_BACKOFF_MAX_MS);
        return;
    }

    if (s_attempt < s_max_attempts) {
        s_attempt++;
        timer_arm(TMR_CONNECT, WIFI_MGR_RETRY_DELAY_MS);
        return;
    }
    sta_sequence_failed(reason);
}

/* ======================================================================
 *                         Обробка повідомлень
 * ====================================================================== */

static void handle_got_ip(esp_ip4_addr_t ip)
{
    s_sta_has_ip = true;
    timer_disarm(TMR_ATTEMPT_TIMEOUT);
    timer_disarm(TMR_CONNECT);

    char ip_str[16];
    esp_ip4addr_ntoa(&ip, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Отримано IP: %s", ip_str);

    switch (s_state) {
    case WIFI_MGR_STATE_CONNECTING:
    case WIFI_MGR_STATE_RECONNECTING:
        enter_connected();
        break;

    case WIFI_MGR_STATE_PORTAL_RETRY_SAVED:
        ESP_LOGI(TAG, "Збережена мережа знову доступна");
        portal_stop();
        enter_connected();
        break;

    case WIFI_MGR_STATE_PORTAL_TESTING:
        if (wifi_credentials_save(&s_pending) == ESP_OK) {
            portal_status_set(WIFI_MGR_PORTAL_RESULT_SUCCESS, s_pending.ssid, "", ip_str);
        } else {
            portal_status_set(WIFI_MGR_PORTAL_RESULT_SUCCESS, s_pending.ssid,
                              "Увага: дані не вдалося зберегти, після перезавантаження потрібно буде ввести їх знову.",
                              ip_str);
        }
        memset(&s_pending, 0, sizeof(s_pending));
        s_state = WIFI_MGR_STATE_PORTAL_FINISHING;
        timer_disarm(TMR_SAVED_RETRY);
        timer_arm(TMR_PORTAL_SHUTDOWN, WIFI_MGR_PORTAL_SUCCESS_HOLD_MS);
        break;

    default:
        break;
    }
}

static void handle_disconnected(uint16_t reason)
{
    s_sta_has_ip = false;

    switch (s_state) {
    case WIFI_MGR_STATE_CONNECTING:
    case WIFI_MGR_STATE_RECONNECTING:
    case WIFI_MGR_STATE_PORTAL_TESTING:
    case WIFI_MGR_STATE_PORTAL_RETRY_SAVED:
        if (reason == WIFI_REASON_ASSOC_LEAVE) {
            break;          /* наслідок нашого власного esp_wifi_disconnect() */
        }
        ESP_LOGI(TAG, "Спроба невдала (причина %u)", (unsigned)reason);
        sta_attempt_failed(reason);
        break;

    case WIFI_MGR_STATE_CONNECTED:
        ESP_LOGW(TAG, "З'єднання з роутером втрачено (причина %u)", (unsigned)reason);
        xEventGroupClearBits(s_events, EVT_CONNECTED_BIT);
        post_app_event(APP_WIFI_EVENT_DISCONNECTED);
        enter_reconnecting();
        break;

    default:
        break;              /* PORTAL / FINISHING: STA не використовується */
    }
}

static void handle_reset_credentials(void)
{
    wifi_credentials_erase();
    timer_disarm(TMR_SAVED_RETRY);

    if (is_portal_state(s_state)) {
        timer_disarm(TMR_CONNECT);
        timer_disarm(TMR_ATTEMPT_TIMEOUT);
        timer_disarm(TMR_PORTAL_SHUTDOWN);
        esp_wifi_disconnect();
        memset(&s_pending, 0, sizeof(s_pending));
        s_state = WIFI_MGR_STATE_PORTAL;
        portal_status_set(WIFI_MGR_PORTAL_RESULT_IDLE, "", "", "");
    } else {
        go_to_portal();
    }
}

static void handle_msg(mgr_msg_t *msg)
{
    switch (msg->type) {
    case MSG_STA_GOT_IP:
        handle_got_ip(msg->data.ip);
        break;

    case MSG_STA_DISCONNECTED:
        handle_disconnected(msg->data.reason);
        break;

    case MSG_BUTTON_EDGE:
        timer_arm(TMR_BUTTON_DEBOUNCE, WIFI_MGR_RESET_DEBOUNCE_MS);
        break;

    case MSG_CMD_PORTAL_SUBMIT:
        if (s_state != WIFI_MGR_STATE_PORTAL && s_state != WIFI_MGR_STATE_PORTAL_TESTING &&
            s_state != WIFI_MGR_STATE_PORTAL_RETRY_SAVED) {
            break;
        }
        s_pending = msg->data.creds;
        ESP_LOGI(TAG, "Перевірка мережі «%s», введеної на порталі", s_pending.ssid);
        s_state = WIFI_MGR_STATE_PORTAL_TESTING;
        timer_disarm(TMR_SAVED_RETRY);
        sta_sequence_begin(&s_pending, WIFI_MGR_PORTAL_TEST_ATTEMPTS);
        break;

    case MSG_CMD_SCAN:
        if (s_state == WIFI_MGR_STATE_PORTAL) {
            do_scan();      /* під час спроб підключення сканувати не можна — віддаємо кеш */
        }
        xEventGroupSetBits(s_events, EVT_SCAN_DONE_BIT);
        break;

    case MSG_CMD_START_PORTAL:
        if (!is_portal_state(s_state)) {
            go_to_portal();
        }
        break;

    case MSG_CMD_RESET_CREDENTIALS:
        handle_reset_credentials();
        break;
    }
    memset(msg, 0, sizeof(*msg));   /* не залишати пароль у стеку */
}

static void handle_timer(mgr_timer_t t)
{
    switch (t) {
    case TMR_CONNECT:
        sta_attempt_start();
        break;

    case TMR_ATTEMPT_TIMEOUT:
        ESP_LOGW(TAG, "Таймаут спроби підключення");
        esp_wifi_disconnect();
        sta_attempt_failed(MGR_REASON_TIMEOUT);
        break;

    case TMR_PORTAL_SHUTDOWN:
        if (s_state != WIFI_MGR_STATE_PORTAL_FINISHING) {
            break;
        }
        portal_stop();
        if (s_sta_has_ip) {
            enter_connected();
        } else {
            enter_reconnecting();       /* зв'язок зник за час утримання порталу */
        }
        break;

    case TMR_SAVED_RETRY: {
        if (s_state != WIFI_MGR_STATE_PORTAL) {
            break;
        }
        wifi_sta_list_t clients;
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
            ESP_LOGI(TAG, "До порталу підключено клієнтів: %d — фонова спроба відкладена", clients.num);
            timer_arm(TMR_SAVED_RETRY, WIFI_MGR_PORTAL_SAVED_RETRY_MS);
            break;
        }
        wifi_credentials_t creds;
        if (wifi_credentials_load(&creds) != ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "Фонова спроба підключення до збереженої мережі «%s»", creds.ssid);
        s_state = WIFI_MGR_STATE_PORTAL_RETRY_SAVED;
        sta_sequence_begin(&creds, WIFI_MGR_PORTAL_SAVED_RETRY_ATTEMPTS);
        memset(&creds, 0, sizeof(creds));
        break;
    }

    case TMR_BUTTON_DEBOUNCE: {
        s_btn_msg_pending = false;      /* знову дозволити повідомлення з ISR */
        const bool pressed = (gpio_get_level(WIFI_MGR_RESET_GPIO) == WIFI_MGR_RESET_ACTIVE_LEVEL);
        if (pressed && !timer_is_armed(TMR_BUTTON_HOLD)) {
            timer_arm(TMR_BUTTON_HOLD, WIFI_MGR_RESET_HOLD_MS);
        } else if (!pressed) {
            timer_disarm(TMR_BUTTON_HOLD);
        }
        break;
    }

    case TMR_BUTTON_HOLD:
        if (gpio_get_level(WIFI_MGR_RESET_GPIO) == WIFI_MGR_RESET_ACTIVE_LEVEL) {
            ESP_LOGW(TAG, "Кнопку скидання утримано — стирання даних Wi-Fi і запуск порталу");
            handle_reset_credentials();
        }
        break;

    default:
        break;
    }
}

static void timers_process(void)
{
    const TickType_t now = xTaskGetTickCount();
    for (int t = 0; t < TMR_COUNT; t++) {
        if (s_tmr_armed[t] && (int32_t)(s_tmr_deadline[t] - now) <= 0) {
            s_tmr_armed[t] = false;
            handle_timer((mgr_timer_t)t);
        }
    }
}

/* ======================================================================
 *                         Задача менеджера
 * ====================================================================== */

static void mgr_boot(void)
{
    wifi_credentials_t creds;
    if (wifi_credentials_load(&creds) == ESP_OK) {
        ESP_LOGI(TAG, "Знайдено збережену мережу «%s» — підключення", creds.ssid);
        wifi_apply_mode(WIFI_MODE_STA);
        s_state = WIFI_MGR_STATE_CONNECTING;
        sta_sequence_begin(&creds, WIFI_MGR_BOOT_CONNECT_ATTEMPTS);
    } else {
        ESP_LOGI(TAG, "Збережених даних Wi-Fi немає — запуск Captive Portal");
        portal_start();
    }
    memset(&creds, 0, sizeof(creds));
}

static void mgr_task(void *arg)
{
    (void)arg;
    mgr_boot();

    mgr_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, timer_next_wait()) == pdTRUE) {
            handle_msg(&msg);
        }
        timers_process();
    }
}

/* ======================================================================
 *                 Обробники подій драйвера та кнопки
 * ====================================================================== */

static void post_msg(const mgr_msg_t *msg)
{
    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Черга менеджера переповнена, подію %d втрачено", (int)msg->type);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    mgr_msg_t msg = {0};

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
            msg.type = MSG_STA_DISCONNECTED;
            msg.data.reason = e->reason;
            post_msg(&msg);
            break;
        }
        case WIFI_EVENT_AP_START:
            xEventGroupSetBits(s_events, EVT_AP_STARTED_BIT);
            break;
        case WIFI_EVENT_AP_STOP:
            xEventGroupClearBits(s_events, EVT_AP_STARTED_BIT);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "До порталу підключився клієнт " MACSTR, MAC2STR(e->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "Клієнт " MACSTR " відключився від порталу", MAC2STR(e->mac));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        msg.type = MSG_STA_GOT_IP;
        msg.data.ip = e->ip_info.ip;
        post_msg(&msg);
    }
}

static void reset_button_isr(void *arg)
{
    (void)arg;
    if (s_btn_msg_pending) {
        return;             /* брязкіт контактів: одне повідомлення на серію фронтів */
    }
    s_btn_msg_pending = true;
    mgr_msg_t msg = { .type = MSG_BUTTON_EDGE };
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &msg, &woken) != pdTRUE) {
        s_btn_msg_pending = false;
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void reset_button_init(void)
{
    const gpio_num_t pin = WIFI_MGR_RESET_GPIO;
    if (pin == GPIO_NUM_NC) {
        return;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (WIFI_MGR_RESET_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (WIFI_MGR_RESET_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;   /* сервіс уже встановлений іншим модулем */
        }
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(pin, reset_button_isr, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Кнопку скидання не налаштовано: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Кнопка скидання Wi-Fi: GPIO%d, утримання %d мс", (int)pin, WIFI_MGR_RESET_HOLD_MS);
    }
}

/* ======================================================================
 *                         Публічний API
 * ====================================================================== */

esp_err_t wifi_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "Менеджер уже запущений");

    s_queue = xQueueCreate(WIFI_MGR_QUEUE_LENGTH, sizeof(mgr_msg_t));
    s_events = xEventGroupCreate();
    s_data_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_queue && s_events && s_data_mutex, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && s_ap_netif, ESP_FAIL, TAG, "Не вдалося створити netif");

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    /* Дані мережі зберігає наш модуль у NVS, драйверу власне сховище не потрібне */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register WIFI_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register IP_EVENT");

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", WIFI_MGR_AP_SSID_PREFIX, mac[4], mac[5]);

    reset_button_init();

    ESP_RETURN_ON_FALSE(xTaskCreate(mgr_task, "wifi_mgr", WIFI_MGR_TASK_STACK_SIZE, NULL,
                                    WIFI_MGR_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

bool wifi_manager_wait_connected(TickType_t timeout)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupWaitBits(s_events, EVT_CONNECTED_BIT, pdFALSE, pdTRUE, timeout) & EVT_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & EVT_CONNECTED_BIT) != 0;
}

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return s_state;
}

static esp_err_t send_cmd(const mgr_msg_t *msg)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xQueueSend(s_queue, msg, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_start_portal(void)
{
    const mgr_msg_t msg = { .type = MSG_CMD_START_PORTAL };
    return send_cmd(&msg);
}

esp_err_t wifi_manager_reset_credentials(void)
{
    const mgr_msg_t msg = { .type = MSG_CMD_RESET_CREDENTIALS };
    return send_cmd(&msg);
}

esp_err_t wifi_manager_set_power_save(wifi_ps_type_t ps)
{
    return esp_wifi_set_ps(ps);
}

esp_err_t wifi_manager_portal_submit(const char *ssid, const char *password)
{
    if (!wifi_credentials_is_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }
    const wifi_mgr_state_t st = s_state;
    if (st != WIFI_MGR_STATE_PORTAL && st != WIFI_MGR_STATE_PORTAL_TESTING &&
        st != WIFI_MGR_STATE_PORTAL_RETRY_SAVED) {
        return ESP_ERR_INVALID_STATE;
    }

    mgr_msg_t msg = { .type = MSG_CMD_PORTAL_SUBMIT };
    strlcpy(msg.data.creds.ssid, ssid, sizeof(msg.data.creds.ssid));
    strlcpy(msg.data.creds.password, password, sizeof(msg.data.creds.password));

    /* Статус "перевірка" ставиться одразу, щоб сторінка не побачила старий результат */
    portal_status_set(WIFI_MGR_PORTAL_RESULT_TESTING, ssid, "", "");
    const esp_err_t err = send_cmd(&msg);
    memset(&msg, 0, sizeof(msg));
    if (err != ESP_OK) {
        portal_status_set(WIFI_MGR_PORTAL_RESULT_FAILED, ssid, "Пристрій зайнятий, спробуйте ще раз.", "");
    }
    return err;
}

esp_err_t wifi_manager_portal_request_scan(TickType_t timeout)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_events, EVT_SCAN_DONE_BIT);
    const mgr_msg_t msg = { .type = MSG_CMD_SCAN };
    ESP_RETURN_ON_ERROR(send_cmd(&msg), TAG, "Запит сканування не прийнято");
    const EventBits_t bits = xEventGroupWaitBits(s_events, EVT_SCAN_DONE_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & EVT_SCAN_DONE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

size_t wifi_manager_portal_get_scan_results(wifi_mgr_scan_entry_t *out, size_t max_entries)
{
    if (out == NULL || s_data_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    const size_t n = MIN(s_scan_count, max_entries);
    memcpy(out, s_scan_cache, n * sizeof(out[0]));
    xSemaphoreGive(s_data_mutex);
    return n;
}

void wifi_manager_portal_get_status(wifi_mgr_portal_status_t *out)
{
    if (out == NULL || s_data_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    *out = s_portal_status;
    xSemaphoreGive(s_data_mutex);
}
