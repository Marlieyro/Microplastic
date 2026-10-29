//
// Created by user on 27.10.26.
//
// Captive Portal: DHCP-опція 114 + DNS-сервер + HTTP-сервер сторінки налаштування.
//
// Маршрути HTTP:
//   GET  /                    — сторінка налаштування (root.html)
//   GET  /scan[?refresh=1]    — JSON-список мереж [{"ssid":"..","rssi":-50,"open":false}]
//   POST /connect             — form-urlencoded: ssid=..&password=..
//   GET  /status              — JSON-статус перевірки {"state":"idle|testing|ok|fail",...}
//   GET  /captive-portal/api  — RFC 8908 (адреса передається через DHCP-опцію 114)
//   будь-що інше              — 302 на http://<IP AP>/ — саме це змушує телефони
//                               та ноутбуки (Android, iOS, Windows, macOS, Linux,
//                               Firefox) показати вікно "Увійдіть у мережу".
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "appCaptivePortal.h"
#include "appDNSServer.h"
#include "appWiFiManager.h"
#include "appWiFiCredentials.h"
#include "appwifiConfig.h"

#define PORTAL_API_URI          "/captive-portal/api"
#define PORTAL_FORM_MAX_LEN     512
#define PORTAL_SCAN_WAIT_MS     10000
#define PORTAL_RECV_RETRIES     3
#define PORTAL_HTTPD_STACK      6144
/* 3 сокети резервує сам httpd, 1 — DNS-сервер, 1 — запас */
#define PORTAL_HTTPD_MAX_SOCKETS \
    (((CONFIG_LWIP_MAX_SOCKETS - 5) < 3) ? 3 : (CONFIG_LWIP_MAX_SOCKETS - 5))

/* Найгірший випадок екранування SSID у JSON: кожен байт -> \u00XX (6 символів) */
#define JSON_SSID_ESC_SIZE      (WIFI_CRED_SSID_MAX_LEN * 6 + 1)

extern const char root_html_start[] asm("_binary_root_html_start");
extern const char root_html_end[]   asm("_binary_root_html_end");

static const char *TAG = "CAPTIVE_PORTAL";

static httpd_handle_t s_server = NULL;
/* Статичні: DHCP-сервер зберігає вказівник на рядок, а не копію */
static char s_portal_url[32];
static char s_api_url[64];

/* ---------------- Допоміжні функції ---------------- */

static size_t json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        char tmp[8];
        size_t len;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            len = 2;
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
            len = 6;
        } else {
            tmp[0] = (char)*p;
            len = 1;
        }
        if (n + len >= cap) {
            break;
        }
        memcpy(&dst[n], tmp, len);
        n += len;
    }
    dst[n] = '\0';
    return n;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Декодування application/x-www-form-urlencoded: '+' -> ' ', %XX -> байт */
static void url_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r != '\0') {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            *w++ = (char)((hex_value(r[1]) << 4) | hex_value(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void set_no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
}

static esp_err_t send_result(httpd_req_t *req, const char *status, bool ok, const char *msg)
{
    char esc[256];
    char body[320];
    json_escape(esc, sizeof(esc), msg);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", esc);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    set_no_cache(req);
    return httpd_resp_sendstr(req, body);
}

/* ---------------- Обробники HTTP ---------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    set_no_cache(req);
    return httpd_resp_send(req, root_html_start, root_html_end - root_html_start);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[4];
        if (httpd_query_key_value(query, "refresh", value, sizeof(value)) == ESP_OK && value[0] == '1') {
            wifi_manager_portal_request_scan(pdMS_TO_TICKS(PORTAL_SCAN_WAIT_MS));
        }
    }

    wifi_mgr_scan_entry_t *list = calloc(WIFI_MGR_SCAN_MAX_APS, sizeof(*list));
    if (list == NULL) {
        return httpd_resp_send_500(req);
    }
    const size_t count = wifi_manager_portal_get_scan_results(list, WIFI_MGR_SCAN_MAX_APS);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    set_no_cache(req);
    httpd_resp_sendstr_chunk(req, "[");

    char esc[JSON_SSID_ESC_SIZE];
    char item[JSON_SSID_ESC_SIZE + 64];
    for (size_t i = 0; i < count; i++) {
        json_escape(esc, sizeof(esc), list[i].ssid);
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                 (i > 0) ? "," : "", esc, (int)list[i].rssi, list[i].is_open ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
    }
    free(list);

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    static const char *const state_names[] = { "idle", "testing", "ok", "fail" };

    wifi_mgr_portal_status_t st;
    wifi_manager_portal_get_status(&st);

    char ssid_esc[JSON_SSID_ESC_SIZE];
    char msg_esc[sizeof(st.message) * 2];
    char body[sizeof(ssid_esc) + sizeof(msg_esc) + 96];
    json_escape(ssid_esc, sizeof(ssid_esc), st.ssid);
    json_escape(msg_esc, sizeof(msg_esc), st.message);

    const unsigned idx = (unsigned)st.result < 4 ? (unsigned)st.result : 0;
    snprintf(body, sizeof(body), "{\"state\":\"%s\",\"ssid\":\"%s\",\"msg\":\"%s\",\"ip\":\"%s\"}",
             state_names[idx], ssid_esc, msg_esc, st.ip);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    set_no_cache(req);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > PORTAL_FORM_MAX_LEN) {
        return send_result(req, "400 Bad Request", false, "Некоректний запит.");
    }

    char body[PORTAL_FORM_MAX_LEN + 1];
    size_t received = 0;
    int retries = 0;
    while (received < req->content_len) {
        const int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= PORTAL_RECV_RETRIES) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    /* Буфери з запасом під %XX-кодування (до 3 символів на байт) */
    char ssid[WIFI_CRED_SSID_MAX_LEN * 3 + 1];
    char pass[WIFI_CRED_PASS_MAX_LEN * 3 + 1];

    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        memset(body, 0, sizeof(body));
        return send_result(req, "400 Bad Request", false, "Вкажіть назву мережі (SSID).");
    }
    const esp_err_t pass_err = httpd_query_key_value(body, "password", pass, sizeof(pass));
    memset(body, 0, sizeof(body));
    if (pass_err == ESP_ERR_NOT_FOUND) {
        pass[0] = '\0';
    } else if (pass_err != ESP_OK) {
        return send_result(req, "400 Bad Request", false, "Пароль задовгий.");
    }

    url_decode_inplace(ssid);
    url_decode_inplace(pass);

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (wifi_credentials_is_valid(ssid, pass)) {
        err = wifi_manager_portal_submit(ssid, pass);
    }
    memset(pass, 0, sizeof(pass));

    switch (err) {
    case ESP_OK:
        return send_result(req, "200 OK", true, "");
    case ESP_ERR_INVALID_ARG:
        return send_result(req, "400 Bad Request", false,
                           "Некоректні дані: SSID 1–32 байти, пароль порожній або 8–63 символи.");
    case ESP_ERR_INVALID_STATE:
        return send_result(req, "409 Conflict", false, "Пристрій зараз не в режимі налаштування.");
    default:
        return send_result(req, "503 Service Unavailable", false, "Пристрій зайнятий, спробуйте ще раз.");
    }
}

/* RFC 8908: Captive Portal API */
static esp_err_t api_get_handler(httpd_req_t *req)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"captive\":true,\"user-portal-url\":\"%s\"}", s_portal_url);
    httpd_resp_set_type(req, "application/captive+json");
    set_no_cache(req);
    return httpd_resp_sendstr(req, body);
}

/* Будь-яка невідома адреса -> перенаправлення на сторінку порталу */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    set_no_cache(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* iOS вимагає непорожнє тіло відповіді, інакше портал не розпізнається */
    return httpd_resp_sendstr(req, "<html><body>Redirect to the captive portal</body></html>");
}

static const httpd_uri_t s_uri_root    = { .uri = "/",            .method = HTTP_GET,  .handler = root_get_handler };
static const httpd_uri_t s_uri_scan    = { .uri = "/scan",        .method = HTTP_GET,  .handler = scan_get_handler };
static const httpd_uri_t s_uri_status  = { .uri = "/status",      .method = HTTP_GET,  .handler = status_get_handler };
static const httpd_uri_t s_uri_connect = { .uri = "/connect",     .method = HTTP_POST, .handler = connect_post_handler };
static const httpd_uri_t s_uri_api     = { .uri = PORTAL_API_URI, .method = HTTP_GET,  .handler = api_get_handler };

/* ---------------- DHCP ---------------- */

static void portal_configure_dhcp(esp_netif_t *ap_netif)
{
    /* DNS-сервер у DHCP-відповіді за замовчуванням = IP точки доступу (ESP-IDF v6),
     * тому достатньо додати лише опцію 114 (RFC 8910). */
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_api_url, strlen(s_api_url));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP-опцію 114 не встановлено: %s", esp_err_to_name(err));
    }
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start: %s", esp_err_to_name(err));
    }
}

/* ---------------- Публічний API ---------------- */

esp_err_t captive_portal_start(esp_netif_t *ap_netif)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(ap_netif != NULL, ESP_ERR_INVALID_ARG, TAG, "ap_netif == NULL");

    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ip_info), TAG, "Немає IP точки доступу");

    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    snprintf(s_portal_url, sizeof(s_portal_url), "http://%s/", ip_str);
    snprintf(s_api_url, sizeof(s_api_url), "http://%s%s", ip_str, PORTAL_API_URI);

    portal_configure_dhcp(ap_netif);

    ESP_RETURN_ON_ERROR(dns_server_start(ip_info.ip), TAG, "DNS-сервер не запущено");

    /* Перенаправлення генерують багато "сміттєвих" запитів — приглушуємо логи httpd */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = PORTAL_HTTPD_STACK;
    config.max_open_sockets = PORTAL_HTTPD_MAX_SOCKETS;
    config.lru_purge_enable = true;     /* ноутбуки відкривають багато з'єднань */
    config.max_uri_handlers = 8;

    const esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP-сервер порталу не запущено: %s", esp_err_to_name(err));
        s_server = NULL;
        dns_server_stop();
        return err;
    }

    httpd_register_uri_handler(s_server, &s_uri_root);
    httpd_register_uri_handler(s_server, &s_uri_scan);
    httpd_register_uri_handler(s_server, &s_uri_status);
    httpd_register_uri_handler(s_server, &s_uri_connect);
    httpd_register_uri_handler(s_server, &s_uri_api);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_handler);

    ESP_LOGI(TAG, "Портал запущено: %s", s_portal_url);
    return ESP_OK;
}

void captive_portal_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    dns_server_stop();

    esp_log_level_set("httpd_uri", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set("httpd_txrx", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    esp_log_level_set("httpd_parse", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
}
