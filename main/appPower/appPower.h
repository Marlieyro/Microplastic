//
// Профілі енергоспоживання (без вимкнення Wi-Fi).
//   NORMAL: CPU 160 МГц фіксовано, Wi-Fi modem sleep MIN.
//   LOW   : auto light sleep + DFS (40..160 МГц) + Wi-Fi modem sleep MAX.
//           Чип засинає, коли всі задачі заблоковані, і прокидається на маяк
//           точки доступу (DTIM), таймер або натискання кнопки (GPIO1, низький рівень).
//

#ifndef MICROPLASTIC_APPPOWER_H
#define MICROPLASTIC_APPPOWER_H

#include <stdbool.h>
#include "esp_err.h"

/** @brief Налаштовує пробудження кнопкою і утримання рівнів GPIO у light sleep. */
void app_power_init(void);

/** @brief Застосовує профіль: low = true — light sleep + DFS + modem sleep MAX. */
esp_err_t app_power_apply(bool low);

/** @brief Чи активний енергоощадний профіль. */
bool app_power_is_low(void);

#endif //MICROPLASTIC_APPPOWER_H
