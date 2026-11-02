//
// Індикація стану системи сигнальним світлодіодом (GPIO4, D2).
//

#ifndef MICROPLASTIC_APPLED_H
#define MICROPLASTIC_APPLED_H

#include "esp_err.h"

/** @brief Налаштовує GPIO і створює задачу індикації. */
esp_err_t app_led_start(void);

#endif //MICROPLASTIC_APPLED_H
