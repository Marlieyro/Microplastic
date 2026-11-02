//
// Created by user on 29.10.26.
//

#ifndef MICROPLASTIC_APPSENSOR_SENSOR_H
#define MICROPLASTIC_APPSENSOR_SENSOR_H

#include <stdbool.h>
#include "esp_err.h"
#include "as7341.h"

/** @brief Налаштовує GPIO світлодіодів збудження (IR 940 нм, Blue 470 нм) і вимикає їх.
 *         Викликається завжди, щоб піни не «висіли» в невизначеному стані. */
void appSensor_GpioInit(void);

/** @brief Ініціалізація AS7341 (лише APP_DATA_SOURCE_SIM = 0). Потребує appI2C_Init(). */
esp_err_t appSensor_Config(void);

/** @brief Дескриптор AS7341 або NULL, якщо сенсор не ініціалізовано. */
as7341_handle_t appSensor_GetHandle(void);

/**
 * @brief Один повний цикл вимірювання: 32 x (DARK -> IR -> BLUE), результат —
 *        у чергу сирих даних. Тривалість ≈ 15–17 с.
 * @param abort_flag true — перервати цикл (stop_work).
 * @return ESP_OK; ESP_ERR_INVALID_STATE — перервано; інше — помилка I2C/черги.
 */
esp_err_t appSensorStart_Work(as7341_handle_t dev_hdl, volatile const bool *abort_flag);

#endif //MICROPLASTIC_APPSENSOR_SENSOR_H
