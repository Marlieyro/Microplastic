//
// Індикація стану системи сигнальним світлодіодом (GPIO4, D2).
//
// Кожен шаблон — бітова маска на часовій сітці з кроком 50 мс:
// біт i = 1 — LED увімкнено в i-му кроці періоду.
// Задача прокидається ЛИШЕ в моменти зміни рівня (або за app_led_kick()),
// а не кожні 50 мс, — це дозволяє чипу засинати в режимі light sleep.
//
//   Ситуація                         Шаблон
//   INIT                             10 Гц
//   Captive Portal / configuration   подвійний спалах кожні 2 с
//   Підключення до Wi-Fi             2 Гц
//   WAITING                          спалах 50 мс кожні 3 с
//   WORKING                          світиться постійно
//   UPLOADING                        5 Гц
//   UPDATING / режим ota             2 спалахи кожні 0.5 с
//   ERROR                            3 швидкі спалахи, пауза 1.5 с
//   Сигнал «відхилено» (кнопка)      2 короткі спалахи
//   Сигнал «перезапуск»              10 Гц протягом 0.5 с
//

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"

#include "appConfig.h"
#include "appLed.h"
#include "appControl.h"
#include "appModes.h"
#include "appWiFiManager.h"

#define LED_STEP_MS 50

typedef struct {
    uint8_t  period;    /* кроків у періоді (1..64) */
    uint64_t mask;      /* біт i — стан LED у кроці i */
} led_pattern_t;

static const led_pattern_t PAT_INIT       = { 2,  0x1ULL };
static const led_pattern_t PAT_PORTAL     = { 40, 0x33ULL };
static const led_pattern_t PAT_CONNECTING = { 10, 0x1FULL };
static const led_pattern_t PAT_WAITING    = { 60, 0x1ULL };
static const led_pattern_t PAT_WORKING    = { 1,  0x1ULL };
static const led_pattern_t PAT_UPLOADING  = { 4,  0x3ULL };
static const led_pattern_t PAT_UPDATING   = { 10, 0x5ULL };
static const led_pattern_t PAT_ERROR      = { 36, 0x15ULL };
static const led_pattern_t PAT_REJECT     = { 8,  0x5ULL };
static const led_pattern_t PAT_REBOOT     = { 2,  0x1ULL };

static const char *TAG = "LED";

static TaskHandle_t s_task = NULL;
static const led_pattern_t *volatile s_overlay = NULL;
static volatile TickType_t s_overlay_until = 0;

static const led_pattern_t *select_pattern(void)
{
    const led_pattern_t *ov = s_overlay;
    if (ov != NULL) {
        if ((int32_t)(s_overlay_until - xTaskGetTickCount()) > 0) {
            return ov;
        }
        s_overlay = NULL;
    }

    const app_state_t st = app_ctrl_get_state();
    const applicationModes_t mode = getApplicationMode();

    if (st == APP_STATE_ERROR) {
        return &PAT_ERROR;
    }
    if (st == APP_STATE_WORKING) {
        return &PAT_WORKING;
    }
    if (st == APP_STATE_UPDATING || mode == OTA_MODE) {
        return &PAT_UPDATING;
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

static int bit_at(const led_pattern_t *p, uint32_t step)
{
    return (int)((p->mask >> (step % p->period)) & 1ULL);
}

static void led_task(void *arg)
{
    (void)arg;
    const led_pattern_t *current = NULL;
    uint32_t step = 0;

    for (;;) {
        const led_pattern_t *p = select_pattern();
        if (p != current) {
            current = p;
            step = 0;
        }
        const int level = bit_at(current, step);
        gpio_set_level(APP_PIN_LED_STATUS, (uint32_t)level);

        /* Скільки кроків рівень не змінюватиметься */
        uint32_t k = 1;
        while (k < current->period && bit_at(current, step + k) == level) {
            k++;
        }
        if (current->period == 1) {
            k = 20;                     /* постійне світіння: перевірка раз на 1 с */
        }
        if (s_overlay != NULL) {
            const int32_t left = (int32_t)(s_overlay_until - xTaskGetTickCount());
            const uint32_t left_steps = (left > 0) ? (uint32_t)left / pdMS_TO_TICKS(LED_STEP_MS) + 1 : 1;
            if (k > left_steps) {
                k = left_steps;
            }
        }
        step = (step + k) % current->period;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(k * LED_STEP_MS));
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

    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led", APP_TASK_LED_STACK, NULL, APP_TASK_LED_PRIO, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

void app_led_kick(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void app_led_signal(app_led_signal_t sig)
{
    const led_pattern_t *p = (sig == APP_LED_SIGNAL_REBOOT) ? &PAT_REBOOT : &PAT_REJECT;
    const uint32_t dur_ms = (sig == APP_LED_SIGNAL_REBOOT) ? 500 : (uint32_t)p->period * LED_STEP_MS;
    s_overlay_until = xTaskGetTickCount() + pdMS_TO_TICKS(dur_ms);
    s_overlay = p;
    app_led_kick();
}
