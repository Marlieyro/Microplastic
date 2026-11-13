//
// Created by user on 29.10.26.
//
// Керування світлодіодами збудження та сенсором AS7341.
//
// УВАГА: у поточній системі сирі дані надходять ВИКЛЮЧНО з HIL-симулятора
// (APP_DATA_SOURCE_SIM = 1 в appConfig.h). Функції appSensor_Config() та
// appSensorStart_Work() реалізують роботу з фізичним сенсором і викликаються
// лише при APP_DATA_SOURCE_SIM = 0.
//

#include <stdlib.h>
#include <string.h>
#include "appSensor_Sensor.h"
#include "appSensor_I2CConfig.h"
#include "appConfig.h"
#include "as7341.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "appQueueRawDataBuffer.h"
#include "dsp_processor_v1.h"

static const char *TAG = "AS7341";

#define MEASUREMENTS_COUNT  ROWS    /* 32 проходи */
#define LED_SETTLE_MS       50      /* стабілізація світлового потоку LED перед інтеграцією */

typedef enum { PHASE_DARK = 0, PHASE_IR, PHASE_BLUE } phase_t;

static as7341_handle_t dev_hdl_t = NULL;

static void map_spectral_data_to_array(uint16_t *row_buf, const as7341_channels_spectral_data_t *data)
{
    row_buf[0] = data->f1;
    row_buf[1] = data->f2;
    row_buf[2] = data->f3;
    row_buf[3] = data->f4;
    row_buf[4] = data->f5;
    row_buf[5] = data->f6;
    row_buf[6] = data->f7;
    row_buf[7] = data->f8;
    row_buf[8] = data->clear;
    row_buf[9] = data->nir;
}

/* Поелементний запис у packed-структуру (без вказівників на її поля) */
static void store_row(SensorData *sd, phase_t phase, int row, const uint16_t *vals)
{
    for (int c = 0; c < COLS; c++) {
        switch (phase) {
        case PHASE_DARK: sd->dark_buf[row][c] = vals[c]; break;
        case PHASE_IR:   sd->ir_buf[row][c]   = vals[c]; break;
        case PHASE_BLUE: sd->blue_buf[row][c] = vals[c]; break;
        }
    }
}

static void leds_set(int ir, int blue)
{
    gpio_set_level(APP_PIN_LED_IR, ir);
    gpio_set_level(APP_PIN_LED_BLUE, blue);
}

void appSensor_GpioInit(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << APP_PIN_LED_IR) | (1ULL << APP_PIN_LED_BLUE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    leds_set(0, 0);
}

esp_err_t appSensor_Config(void)
{
    as7341_config_t dev_cfg = AS7341_CONFIG_DEFAULT;   /* адреса 0x39, I2C 100 кГц */

    /* T_int = (ATIME+1)(ASTEP+1) * 2.78 мкс = 30 * 600 * 2.78 мкс ≈ 50 мс;
     * максимальний відлік АЦП = (ATIME+1)(ASTEP+1) = 18000 (ADC_CLIP_LIMIT у DSP) */
    dev_cfg.atime = 29;
    dev_cfg.astep = 599;
    dev_cfg.spectral_gain = AS7341_SPECTRAL_GAIN_16X;

    const esp_err_t ret = as7341_init(get_i2c_bus_handle(), &dev_cfg, &dev_hdl_t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Помилка ініціалізації AS7341: %s", esp_err_to_name(ret));
        dev_hdl_t = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "AS7341 успішно ініціалізовано");
    return ESP_OK;
}

as7341_handle_t appSensor_GetHandle(void)
{
    return dev_hdl_t;
}

static esp_err_t measure_phase(as7341_handle_t dev_hdl, SensorData *sd, phase_t phase, int row)
{
    as7341_channels_spectral_data_t adc_data;
    uint16_t temp_row[COLS];


    // TODO первірити факт того чи реально потірбна затримка, оскільки робочий режим LED досягається за наносекунди
    // TODO видалити затримку
    vTaskDelay(pdMS_TO_TICKS(LED_SETTLE_MS));
    const esp_err_t err = as7341_get_spectral_measurements(dev_hdl, &adc_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Помилка вимірювання (фаза %d, прохід %d): %s", (int)phase, row, esp_err_to_name(err));
        return err;
    }
    map_spectral_data_to_array(temp_row, &adc_data);
    store_row(sd, phase, row, temp_row);
    return ESP_OK;
}

// Затримка ≈ 15–17 с на отримання всього кадру
esp_err_t appSensorStart_Work(as7341_handle_t dev_hdl, volatile const bool *abort_flag)
{
    SensorData *sensor_data = (SensorData *)calloc(1, sizeof(SensorData));
    if (sensor_data == NULL) {
        ESP_LOGE(TAG, "Помилка виділення пам'яті (Heap Full)");
        return ESP_ERR_NO_MEM;
    }

    // TODO прибрати температуру взагалі (AS7341 не має датчика температури)
    /*sensor_data->temp_dark = 0.0f;
    sensor_data->temp_ir = 0.0f;
    sensor_data->temp_blue = 0.0f;*/

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < MEASUREMENTS_COUNT && ret == ESP_OK; i++) {
        if (abort_flag != NULL && *abort_flag) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        // --- ФАЗА 1: Темнова (всі світлодіоди вимкнено) ---
        leds_set(0, 0);
        ret = measure_phase(dev_hdl, sensor_data, PHASE_DARK, i);

        // --- ФАЗА 2: Інфрачервона / Каламутність ---
        if (ret == ESP_OK) {
            leds_set(1, 0);
            ret = measure_phase(dev_hdl, sensor_data, PHASE_IR, i);
        }

        // --- ФАЗА 3: Синя / Мікропластик ---
        if (ret == ESP_OK) {
            leds_set(0, 1);
            ret = measure_phase(dev_hdl, sensor_data, PHASE_BLUE, i);
        }
    }

    // Вимикаємо всі LED після завершення циклу (або помилки)
    leds_set(0, 0);

    if (ret == ESP_OK) {
        // xQueueSend виконує глибоку копію (sizeof(SensorData) байтів) у буфер черги
        if (xQueueSend(getQueueRawDataBuffer(), sensor_data, pdMS_TO_TICKS(1000)) != pdPASS) {
            ESP_LOGW(TAG, "Черга сирих даних переповнена — кадр втрачено");
            ret = ESP_ERR_TIMEOUT;
        } else {
            ESP_LOGI(TAG, "Кадр (32 проходи x 3 фази) передано у чергу DSP");
        }
    }

    free(sensor_data);
    return ret;
}
