//
// Created by user on 28.10.26.
//

#include "appSensor_I2CConfig.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

// Налаштування пінів (можна змінити на ті, що розведені на твоїй платі)
// TODO піни
#define I2C_MASTER_SCL_IO           9
#define I2C_MASTER_SDA_IO           8
#define I2C_MASTER_NUM              0      // Номер I2C порту (0 або 1)
#define I2C_MASTER_FREQ_HZ          400000 // Частота шини (Fast Mode - 400kH)
#define I2C_SLAVE_ADRR              (0x39)

static const char *TAG = "i2c_master";

i2c_master_bus_handle_t i2c_bus_hdl = {0};
i2c_master_dev_handle_t dev_handle = {0};

static void appI2C_Config(void) {
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &i2c_bus_hdl));
    ESP_LOGI(TAG, "I2C шина успішно ініціалізована");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_SLAVE_ADRR, // Slave adrr
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_hdl, &dev_cfg, &dev_handle));
    ESP_LOGI(TAG, "Пристрій успішно додано на шину");
}

void appI2C_Init(void) {
    appI2C_Config();
}

i2c_master_bus_handle_t get_i2c_bus_handle(void) {
    return i2c_bus_hdl;
}

i2c_master_dev_handle_t get_dev_handle(void) {
    return dev_handle;
}