//
// Збереження облікових даних Wi-Fi (SSID + пароль) в NVS.
//
// NVS має бути ініціалізований (nvs_flash_init) до виклику цих функцій —
// це робиться один раз у main.c -> system_core_init().
//

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs.h"
#include "appWiFiCredentials.h"

#define WIFI_CRED_NVS_NAMESPACE   "wifi_creds"
#define WIFI_CRED_NVS_KEY_SSID    "ssid"
#define WIFI_CRED_NVS_KEY_PASS    "pass"

static const char *TAG = "WIFI_CREDS";

bool wifi_credentials_is_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return false;
    }

    const size_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len > WIFI_CRED_SSID_MAX_LEN) {
        return false;
    }

    const size_t pass_len = strlen(password);
    if (pass_len == 0) {
        return true;                                    /* відкрита мережа */
    }
    if (pass_len >= WIFI_CRED_PASS_MIN_LEN && pass_len < WIFI_CRED_PASS_MAX_LEN) {
        return true;                                    /* парольна фраза 8..63 */
    }
    if (pass_len == WIFI_CRED_PASS_MAX_LEN) {           /* сирий PSK: 64 hex */
        for (size_t i = 0; i < pass_len; i++) {
            if (!isxdigit((unsigned char)password[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

esp_err_t wifi_credentials_load(wifi_credentials_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;     /* ESP_ERR_NVS_NOT_FOUND, якщо дані ще жодного разу не зберігались */
    }

    size_t len = sizeof(out->ssid);
    err = nvs_get_str(handle, WIFI_CRED_NVS_KEY_SSID, out->ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(out->password);
        err = nvs_get_str(handle, WIFI_CRED_NVS_KEY_PASS, out->password, &len);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }
    if (!wifi_credentials_is_valid(out->ssid, out->password)) {
        ESP_LOGW(TAG, "Збережені дані Wi-Fi пошкоджені — ігноруються");
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t wifi_credentials_save(const wifi_credentials_t *creds)
{
    if (creds == NULL || !wifi_credentials_is_valid(creds->ssid, creds->password)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_CRED_NVS_KEY_SSID, creds->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CRED_NVS_KEY_PASS, creds->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Помилка збереження даних Wi-Fi: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Дані Wi-Fi для мережі «%s» збережено в NVS", creds->ssid);
    }
    return err;
}

esp_err_t wifi_credentials_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Помилка стирання даних Wi-Fi: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "Дані Wi-Fi стерто з NVS");
    }
    return err;
}
