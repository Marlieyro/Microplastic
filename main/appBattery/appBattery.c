//
// Датчик заряду батареї MAX17043 (Maxim/Analog Devices, ModelGauge).
//
// Регістри (16 біт, старший байт першим; читати обидва байти в одній транзакції):
//   0x02 VCELL   — 12-бітна напруга (біти 15..4), 1.25 мВ/LSB (MAX17043, 1S);
//   0x04 SOC     — заряд: старший байт = %, молодший = 1/256 %;
//   0x08 VERSION — версія мікросхеми (перевірка присутності).
// Алгоритм ModelGauge оцінює SOC за моделлю Li-ion елемента без шунта струму.
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"

#include "appConfig.h"
#include "appBattery.h"
#include "appControl.h"
#include "appSensor_I2CConfig.h"

#define REG_VCELL       0x02
#define REG_SOC         0x04
#define REG_VERSION     0x08
#define VCELL_UV_LSB    1250        /* 1.25 мВ для MAX17043 (MAX17044 — 2.50 мВ) */
#define I2C_TIMEOUT_MS  100

static const char *TAG = "BATTERY";

static i2c_master_dev_handle_t s_dev = NULL;
static volatile bool  s_valid = false;
static volatile float s_percent = 0.0f;
static volatile float s_mv = 0.0f;

static esp_err_t read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = { 0 };
    const esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof(rx), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *out = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

static void battery_poll(void)
{
    uint16_t soc_raw = 0, vcell_raw = 0;
    if (read_reg(REG_SOC, &soc_raw) != ESP_OK || read_reg(REG_VCELL, &vcell_raw) != ESP_OK) {
        if (s_valid) {
            ESP_LOGW(TAG, "MAX17043 не відповідає");
        }
        s_valid = false;
        return;
    }
    float pct = (float)(soc_raw >> 8) + (float)(soc_raw & 0xFF) / 256.0f;
    if (pct > 100.0f) {
        pct = 100.0f;                       /* ModelGauge може дати трохи > 100 % на повному заряді */
    }
    s_percent = pct;
    s_mv = (float)((vcell_raw >> 4) * VCELL_UV_LSB) / 1000.0f;
    s_valid = true;
    app_ctrl_notify_battery(pct);
}

static void battery_task(void *arg)
{
    (void)arg;
    for (;;) {
        battery_poll();
        vTaskDelay(pdMS_TO_TICKS(APP_BATT_POLL_MS));
    }
}

esp_err_t app_battery_start(void)
{
    i2c_master_bus_handle_t bus = get_i2c_bus_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "Шина I2C не ініціалізована");

    if (i2c_master_probe(bus, APP_BATT_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "MAX17043 не знайдено за адресою 0x%02X — заряд невідомий", APP_BATT_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_BATT_I2C_ADDR,
        .scl_speed_hz = APP_BATT_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "i2c_master_bus_add_device");

    uint16_t version = 0;
    if (read_reg(REG_VERSION, &version) == ESP_OK) {
        ESP_LOGI(TAG, "MAX17043 знайдено, VERSION=0x%04X", version);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(battery_task, "battery", APP_TASK_BATT_STACK, NULL, APP_TASK_BATT_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

bool app_battery_get(float *percent, float *millivolts)
{
    if (!s_valid) {
        return false;
    }
    if (percent != NULL) {
        *percent = s_percent;
    }
    if (millivolts != NULL) {
        *millivolts = s_mv;
    }
    return true;
}
