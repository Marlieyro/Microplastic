//
// Збереження облікових даних Wi-Fi (SSID + пароль) в NVS.
//

#ifndef MICROPLASTIC_APPWIFICREDENTIALS_H
#define MICROPLASTIC_APPWIFICREDENTIALS_H

#include <stdbool.h>
#include "esp_err.h"

/* Обмеження стандарту IEEE 802.11 / WPA2 */
#define WIFI_CRED_SSID_MAX_LEN      32   /* байт */
#define WIFI_CRED_PASS_MIN_LEN      8    /* символів для WPA/WPA2/WPA3-PSK */
#define WIFI_CRED_PASS_MAX_LEN      64   /* 63 ASCII або 64 hex (сирий PSK) */

typedef struct {
    char ssid[WIFI_CRED_SSID_MAX_LEN + 1];
    char password[WIFI_CRED_PASS_MAX_LEN + 1];
} wifi_credentials_t;

/**
 * @brief Перевіряє коректність SSID і пароля.
 *        SSID: 1..32 байти. Пароль: порожній (відкрита мережа),
 *        8..63 символи або рівно 64 hex-символи.
 */
bool wifi_credentials_is_valid(const char *ssid, const char *password);

/**
 * @brief Зчитує збережені дані з NVS.
 * @return ESP_OK — дані є і коректні; ESP_ERR_NVS_NOT_FOUND — даних немає;
 *         інший код — помилка NVS або пошкоджені дані.
 */
esp_err_t wifi_credentials_load(wifi_credentials_t *out);

/** @brief Записує дані в NVS (з nvs_commit). */
esp_err_t wifi_credentials_save(const wifi_credentials_t *creds);

/** @brief Повністю стирає збережені дані Wi-Fi з NVS. */
esp_err_t wifi_credentials_erase(void);

#endif //MICROPLASTIC_APPWIFICREDENTIALS_H
