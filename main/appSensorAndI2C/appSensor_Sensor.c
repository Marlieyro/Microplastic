//
// Created by user on 29.10.26.
//
#include "appSensor_Sensor.h"
#include "appSensor_I2CConfig.h"
#include "as7341.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "appQueueRawDataBuffer.h"
#include "dsp_processor_v1.h"

static const char *TAG = "AS7341 Configuration";

// TODO створити таску для вимірювання

#define MEASUREMENTS_COUNT 32
// TODO піни
#define LED_940_PIN        GPIO_NUM_4 // Замініть на ваш реальний пін
#define LED_470_PIN        GPIO_NUM_5 // Замініть на ваш реальний пін

as7341_handle_t dev_hdl_t = NULL;

// Структури для зберігання результатів (ваші 3 матриці 32x10)
as7341_channels_spectral_data_t dark_matrix[MEASUREMENTS_COUNT];
as7341_channels_spectral_data_t turbidity_matrix[MEASUREMENTS_COUNT];
as7341_channels_spectral_data_t plastic_matrix[MEASUREMENTS_COUNT];

void appSensor_Config() {
    gpio_reset_pin(LED_940_PIN);
    gpio_set_direction(LED_940_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_940_PIN, 0); // Вимикаємо на старті

    gpio_reset_pin(LED_470_PIN);
    gpio_set_direction(LED_470_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_470_PIN, 0); // Вимикаємо на старті


    // 3. Ініціалізація сенсора AS7341
    // Використовуємо макрос для базових налаштувань (адреса 0x39, увімкнене живлення)
    as7341_config_t dev_cfg = AS7341_CONFIG_DEFAULT; // Clock 100hz, написати 400hz

    // Перевизначаємо параметри під ваші вимоги
    dev_cfg.atime = 29;
    dev_cfg.astep = 599;
    dev_cfg.spectral_gain = AS7341_SPECTRAL_GAIN_16X;

    // Ініціалізуємо пристрій на створеній I2C шині
    esp_err_t ret = as7341_init(get_i2c_bus_handle(), &dev_cfg, &dev_hdl_t);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Помилка ініціалізації AS7341: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Апаратна частина успішно ініціалізована");
}

// Затримка 15 сек на отрмиання всього
void appSensorStart_Work(as7341_handle_t dev_hdl) {

    SensorData* sensor_data = (SensorData*)malloc(sizeof(SensorData));
    if (sensor_data == NULL) {
        ESP_LOGE("MICROPLASTICS", "Помилка виділення пам'яті (Heap Full)");
        return;
    }

    // TODO прибрати температуру взагалі
    sensor_data->temp_dark = 0.0f;
    sensor_data->temp_ir = 0.0f;
    sensor_data->temp_blue = 0.0f;

    // Налаштування GPIO для світлодіодів (можна винести в загальний init)
    gpio_set_direction(LED_940_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_470_PIN, GPIO_MODE_OUTPUT);

    as7341_channels_spectral_data_t adc_data;

    for (int i = 0; i < MEASUREMENTS_COUNT; i++) {

        uint16_t temp_row[COLS];
        // --- ФАЗА 1: Темнова (всі світлодіоди вимкнено) ---
        gpio_set_level(LED_940_PIN, 0);
        gpio_set_level(LED_470_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (as7341_get_spectral_measurements(dev_hdl, &adc_data) == ESP_OK) { //[cite: 1, 3]
            map_spectral_data_to_array(temp_row, &adc_data);
            memcpy(sensor_data->dark_buf[i], temp_row, sizeof(temp_row));
        }

        // --- ФАЗА 2: Інфрачервона / Каламутність ---
        gpio_set_level(LED_940_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (as7341_get_spectral_measurements(dev_hdl, &adc_data) == ESP_OK) { //[cite: 1, 3]
            map_spectral_data_to_array(temp_row, &adc_data);
            memcpy(sensor_data->ir_buf[i], temp_row, sizeof(temp_row));
        }

        // --- ФАЗА 3: Синя / Мікропластик ---
        gpio_set_level(LED_940_PIN, 0);
        gpio_set_level(LED_470_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (as7341_get_spectral_measurements(dev_hdl, &adc_data) == ESP_OK) { //[cite: 1, 3]
            map_spectral_data_to_array(temp_row, &adc_data);
            memcpy(sensor_data->blue_buf[i], temp_row, sizeof(temp_row));
        }
    }

    // Вимикаємо всі LED після завершення циклу
    gpio_set_level(LED_470_PIN, 0);

    if (getQueueRawDataBuffer() != NULL) {
        // xQueueSend виконує глибоку копію пам'яті (копіює всі sizeof(SensorData) байтів у внутрішній буфер черги)
        if (xQueueSend(getQueueRawDataBuffer(), sensor_data, pdMS_TO_TICKS(200)) != pdPASS) {
            ESP_LOGW("MICROPLASTICS", "Черга переповнена! Дані втрачено. Перевірте, чи вичитує дані dsp_processor");
        } else {
            ESP_LOGI("MICROPLASTICS", "Пакет даних (32 виміри) успішно відправлено у чергу");
        }
    } else ESP_LOGE("MICROPLASTICS", "Чергу не ініціалізовано. Викликайте app_QueueRawDataBuffer_create() раніше");

    free(sensor_data);
}

static void map_spectral_data_to_array(uint16_t* row_buf, const as7341_channels_spectral_data_t* data) {
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


void sensorStartTask() {

    for (;;) {

    }
    vTaskDelete(NULL);
}