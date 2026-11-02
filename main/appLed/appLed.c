//
// Індикація стану системи сигнальним світлодіодом (GPIO4, D2).
//
// Кожен шаблон — це бітова маска на часовій сітці з кроком 50 мс:
// біт i = 1 — LED увімкнено в i-му кроці періоду. Задача щокроку обирає
// шаблон за поточним станом, тому зміна стану відображається одразу.
//
//   Ситуація                         Шаблон
//   INIT                             10 Гц (50 мс увімк / 50 мс вимк)
//   Captive Portal                   подвійний спалах кожні 2 с
//   Підключення до Wi-Fi             2 Гц
//   WAITING                          спалах 50 мс кожні 3 с
//   WORKING                          світиться постійно
//   UPLOADING (вивантаження FLASH)   5 Гц
//   ERROR                            3 швидкі спалахи, пауза 1.5 с
//

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"

#include "appConfig.h"
#include "appLed.h"
#include "appControl.h"
#include "appWiFiManager.h"

#define LED_STEP_MS 50

typedef struct {
    uint8_t  period;    /* кроків у періоді (1..64) */
    uint64_t mask;      /* біт i — стан LED у кроці i */
} led_pattern_t;

static const led_pattern_t PAT_INIT       = { 2,  0x1ULL };
static const led_pattern_t PAT_PORTAL     = { 40, 0x33ULL };          /* кроки 0,1 і 4,5 */
static const led_pattern_t PAT_CONNECTING = { 10, 0x1FULL };          /* 250 мс / 250 мс */
static const led_pattern_t PAT_WAITING    = { 60, 0x1ULL };           /* 50 мс з 3 с */
static const led_pattern_t PAT_WORKING    = { 1,  0x1ULL };
static const led_pattern_t PAT_UPLOADING  = { 4,  0x3ULL };           /* 100 мс / 100 мс */
static const led_pattern_t PAT_ERROR      = { 36, 0x15ULL };          /* кроки 0,2,4 */

static const char *TAG = "LED";

static const led_pattern_t *select_pattern(void)
{
    const app_state_t st = app_ctrl_get_state();

    /* Пріоритет: ERROR > WORKING > UPLOADING > стан Wi-Fi > WAITING/INIT */
    if (st == APP_STATE_ERROR) {
        return &PAT_ERROR;
    }
    if (st == APP_STATE_WORKING) {
        return &PAT_WORKING;
    }
    if (st == APP_STATE_UPLOADING) {
        return &PAT_UPLOADING;
    }
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_STATE_PORTAL:
    case WIFI_MGR_STATE_PORTAL_TESTING:
    case WIFI_MGR_STATE_PORTAL_FINISHING:
    case WIFI_MGR_STATE_PORTAL_RETRY_SAVED:
        return &PAT_PORTAL;
    case WIFI_MGR_STATE_CONNECTING:
    case WIFI_MGR_STATE_RECONNECTING:
        return &PAT_CONNECTING;
    default:
        break;
    }
    return (st == APP_STATE_WAITING) ? &PAT_WAITING : &PAT_INIT;
}

static void led_task(void *arg)
{
    (void)arg;
    const led_pattern_t *current = NULL;
    uint32_t step = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const led_pattern_t *p = select_pattern();
        if (p != current) {
            current = p;
            step = 0;                       /* новий шаблон — з початку періоду */
        }
        gpio_set_level(APP_PIN_LED_STATUS, (int)((current->mask >> step) & 1ULL));
        step = (step + 1) % current->period;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_STEP_MS));
    }
}

esp_err_t app_led_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_PIN_LED_STATUS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_set_level(APP_PIN_LED_STATUS, 0);

    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led", APP_TASK_LED_STACK, NULL, APP_TASK_LED_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}
