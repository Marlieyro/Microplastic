//
// Фізична кнопка (GPIO1 -> GND, внутрішня підтяжка).
//   1 натискання          -> app_ctrl: старт вимірювання (у ERROR — очищення помилки)
//   2 швидкі натискання   -> app_ctrl: скидання Wi-Fi і запуск Captive Portal
//   утримання >= 3 с      -> перезапуск MCU
//

#ifndef MICROPLASTIC_APPBUTTON_H
#define MICROPLASTIC_APPBUTTON_H

#include "esp_err.h"

/** @brief Налаштовує GPIO, переривання і задачу розпізнавання натискань. */
esp_err_t app_button_start(void);

#endif //MICROPLASTIC_APPBUTTON_H
