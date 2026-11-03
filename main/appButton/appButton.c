//
// Фізична кнопка (див. appButton.h).
//
// Переривання налаштоване на НИЗЬКИЙ РІВЕНЬ (не фронт): саме такий тип
// дозволяє кнопці будити чип з auto light sleep (gpio_wakeup_enable).
// ISR лише вимикає переривання і будить задачу; далі задача опитує пін
// кожні 10 мс, фільтрує брязкіт (3 однакові відліки) і розпізнає жест:
//
//   натиснуто ──(відпущено)──► чекати 400 мс ──(немає 2-го)──► SINGLE
//       │                           └──(2-ге натискання)──────► DOUBLE
//       └──(утримання 3 с)──► LONG (перезапуск)
//
// Після завершення жесту і відпускання кнопки переривання вмикається знову.
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

#include "appConfig.h"
#include "appButton.h"
#include "appControl.h"
#include "appLed.h"

static const char *TAG = "BUTTON";
static TaskHandle_t s_task = NULL;

static void button_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(APP_PIN_BUTTON);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Рівень після фільтрації брязкоту: true — кнопку натиснуто */
static bool read_debounced(bool last)
{
    int same = 0;
    bool cur = last;
    for (int i = 0; i < APP_BTN_DEBOUNCE_SAMPLES * 4 && same < APP_BTN_DEBOUNCE_SAMPLES; i++) {
        const bool v = (gpio_get_level(APP_PIN_BUTTON) == 0);
        if (v == cur) {
            same++;
        } else {
            cur = v;
            same = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BTN_POLL_MS));
    }
    return cur;
}

typedef enum { GESTURE_NONE = 0, GESTURE_SINGLE, GESTURE_DOUBLE, GESTURE_LONG } gesture_t;

static gesture_t capture_gesture(void)
{
    bool pressed = read_debounced(true);
    if (!pressed) {
        return GESTURE_NONE;                /* імпульс завади */
    }
    int clicks = 0;
    TickType_t t_press = xTaskGetTickCount();
    TickType_t t_release = 0;

    for (;;) {
        const bool now_pressed = read_debounced(pressed);
        const TickType_t now = xTaskGetTickCount();
        if (pressed) {
            if (!now_pressed) {
                pressed = false;
                clicks++;
                t_release = now;
                if (clicks >= 2) {
                    return GESTURE_DOUBLE;
                }
            } else if ((now - t_press) >= pdMS_TO_TICKS(APP_BTN_LONG_MS)) {
                return GESTURE_LONG;
            }
        } else {
            if (now_pressed) {
                pressed = true;
                t_press = now;
            } else if ((now - t_release) >= pdMS_TO_TICKS(APP_BTN_DOUBLE_WINDOW_MS)) {
                return GESTURE_SINGLE;
            }
        }
    }
}

static void wait_release(void)
{
    while (gpio_get_level(APP_PIN_BUTTON) == 0) {
        vTaskDelay(pdMS_TO_TICKS(APP_BTN_POLL_MS));
    }
}

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_intr_enable(APP_PIN_BUTTON);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const gesture_t g = capture_gesture();
        switch (g) {
        case GESTURE_SINGLE:
            ESP_LOGI(TAG, "Одиночне натискання");
            app_ctrl_button(APP_BTN_SINGLE);
            break;
        case GESTURE_DOUBLE:
            ESP_LOGI(TAG, "Подвійне натискання");
            app_ctrl_button(APP_BTN_DOUBLE);
            break;
        case GESTURE_LONG:
            ESP_LOGW(TAG, "Утримання %d мс — перезапуск MCU", APP_BTN_LONG_MS);
            app_led_signal(APP_LED_SIGNAL_REBOOT);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;
        default:
            break;
        }
        wait_release();
    }
}

esp_err_t app_button_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_intr_disable(APP_PIN_BUTTON);

    ESP_RETURN_ON_FALSE(xTaskCreate(button_task, "button", APP_TASK_BTN_STACK, NULL, APP_TASK_BTN_PRIO, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");

    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;                       /* сервіс уже встановлено */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "gpio_install_isr_service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(APP_PIN_BUTTON, button_isr, NULL), TAG, "gpio_isr_handler_add");
    ESP_LOGI(TAG, "Кнопка GPIO%d: 1 натискання — старт, 2 — скидання Wi-Fi, утримання %d с — перезапуск",
             (int)APP_PIN_BUTTON, APP_BTN_LONG_MS / 1000);
    return ESP_OK;
}
