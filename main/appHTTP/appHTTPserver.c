//
// Created by user on 27.10.26.
//
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_check.h"
#include <time.h>
#include <sys/time.h>
#if !CONFIG_IDF_TARGET_LINUX
#include <esp_wifi.h>
#include <esp_system.h>
#include "nvs_flash.h"
#include "esp_eth.h"
#endif  // !CONFIG_IDF_TARGET_LINUX

#include "appHTTPserver.h"
#include "dsp_processor_v1.h"
#include "appBuffers/appQueueRawDataBuffer.h"
#include "appWiFiManager.h"

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)

static const char *TAG = "HTTP Server";

/* An HTTP POST handler
 * Приймає бінарні дані і кидає їх в структуру потім кидає в чергу
 */
static esp_err_t rawData_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;

    // Якщо з мережі прийшло більше даних ніж структура то фігня
    if (total_len != sizeof(SensorData)) {
        ESP_LOGE(TAG, "Expected %d bytes, got %d", sizeof(SensorData), total_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Size mismatch");
        return ESP_FAIL;
    }

    // Стек HTTP сервака 2KB тому виділять в heap память розміро з структуру
    SensorData *incoming_data = (SensorData *)malloc(sizeof(SensorData));
    if (incoming_data == NULL) return ESP_FAIL;

    int received_len = 0;
    int ret;
    // Кастуємо вказівник структури до char* для побайтового запису
    char *buf = (char *)incoming_data;

    while (received_len < total_len) {
        ret = httpd_req_recv(req, buf + received_len, total_len - received_len);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) { continue;}
            free(incoming_data);
            return ESP_FAIL;
        }
        received_len += ret;
    }

    // Передаємо скопійовані дані в чергу FreeRTOS.
    // Функція xQueueSend сама зробить копію цих 1932 байтів у свою внутрішню пам'ять.
    if (xQueueSend(getQueueRawDataBuffer(), incoming_data, pdMS_TO_TICKS(100)) == pdPASS) {
        ESP_LOGI(TAG, "SensorData saved to queue");
        httpd_resp_send(req, "Data OK", HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGE(TAG, "Queue is full");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue full");
    }

    // Дані вже в черзі, тому звільняємо тимчасовий буфер
    free(incoming_data);
    return ESP_OK;
}

static const httpd_uri_t rawData = {
    .uri       = "/rawData",
    .method    = HTTP_POST,
    .handler   = rawData_post_handler,
    .user_ctx  = NULL
};

// Конфігурить сервер та реєструє хендлери, та стартує сервер
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
#if CONFIG_IDF_TARGET_LINUX
    // Setting port as 8001 when building for Linux. Port 80 can be used only by a privileged user in linux.
    // So when a unprivileged user tries to run the application, it throws bind error and the server is not started.
    // Port 8001 can be used by an unprivileged user as well. So the application will not throw bind error and the
    // server will be started.
    config.server_port = 8001;
#endif // !CONFIG_IDF_TARGET_LINUX

    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        //httpd_register_uri_handler(server, &mode);
        httpd_register_uri_handler(server, &rawData);
#if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
#endif
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

#if !CONFIG_IDF_TARGET_LINUX
static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

// Стопає вебсервер webserver_stop()
static void disconnect_handler(void* httpd_handle, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) httpd_handle;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

// Стратує вебсервер
static void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}
#endif // !CONFIG_IDF_TARGET_LINUX


void app_http_server_init(void)
{
    static httpd_handle_t server = NULL;

    /* NVS, esp_netif і event loop ініціалізуються один раз у main.c (system_core_init).
     * Тут сервер НЕ стартує: порт 80 може бути зайнятий Captive Portal.
     * Старт і зупинку робить менеджер Wi-Fi через події. */
#if !CONFIG_IDF_TARGET_LINUX
    ESP_ERROR_CHECK(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_CONNECTED,
                                               &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_WIFI_EVENT, APP_WIFI_EVENT_DISCONNECTED,
                                               &disconnect_handler, &server));
#else
    server = start_webserver();
#endif
}