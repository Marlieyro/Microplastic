//
// Профілі енергоспоживання (див. appPower.h).
//

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "appConfig.h"
#include "appPower.h"
#include "appWiFiManager.h"

static const char *TAG = "POWER";
static volatile bool s_low = false;

void app_power_init(void)
{
    /* Під час light sleep GPIO зберігають звичайну конфігурацію (рівні LED, підтяжка кнопки) */
    gpio_sleep_sel_dis(APP_PIN_LED_STATUS);
    gpio_sleep_sel_dis(APP_PIN_LED_IR);
    gpio_sleep_sel_dis(APP_PIN_LED_BLUE);
    gpio_sleep_sel_dis(APP_PIN_BUTTON);

    /* Натискання кнопки (низький рівень) будить чип з light sleep */
    if (gpio_wakeup_enable(APP_PIN_BUTTON, GPIO_INTR_LOW_LEVEL) == ESP_OK) {
        esp_sleep_enable_gpio_wakeup();
    }
}

esp_err_t app_power_apply(bool low)
{
    esp_err_t err = ESP_OK;

#if CONFIG_PM_ENABLE
    const esp_pm_config_t pm = {
        .max_freq_mhz = APP_CPU_MAX_MHZ,
        .min_freq_mhz = low ? APP_CPU_MIN_MHZ_LOW : APP_CPU_MAX_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = low,
#else
        .light_sleep_enable = false,
#endif
    };
    err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(err));
    }
#if !CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (low) {
        ESP_LOGW(TAG, "Light sleep недоступний: увімкніть CONFIG_FREERTOS_USE_TICKLESS_IDLE (працює лише DFS)");
    }
#endif
#else
    if (low) {
        ESP_LOGW(TAG, "Керування живленням вимкнене: увімкніть CONFIG_PM_ENABLE (працює лише modem sleep)");
    }
#endif

    /* Modem sleep діє лише в режимі STA (у Captive Portal — APSTA — недоступний) */
    if (wifi_manager_is_connected()) {
        const esp_err_t e = wifi_manager_set_power_save(low ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(e));
        }
    }

    s_low = low;
    ESP_LOGI(TAG, "Профіль живлення: %s", low ? "LOW (light sleep + DFS + modem sleep MAX)" : "NORMAL (modem sleep MIN)");
    return err;
}

bool app_power_is_low(void)
{
    return s_low;
}
