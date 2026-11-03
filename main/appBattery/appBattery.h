//
// Датчик заряду батареї MAX17043 (ModelGauge, 1S Li-ion), I2C 0x36.
// Спільна шина I2C з AS7341 (GPIO7/GPIO8).
//

#ifndef MICROPLASTIC_APPBATTERY_H
#define MICROPLASTIC_APPBATTERY_H

#include <stdbool.h>
#include "esp_err.h"

/** @brief Додає MAX17043 на шину I2C (appI2C_Init() вже виконано) і запускає задачу опитування.
 *         Якщо датчик не відповідає — система працює далі, заряд вважається невідомим. */
esp_err_t app_battery_start(void);

/** @brief Останні виміряні значення. @return false — датчик недоступний. */
bool app_battery_get(float *percent, float *millivolts);

#endif //MICROPLASTIC_APPBATTERY_H
