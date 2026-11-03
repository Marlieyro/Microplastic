//
// Індикація стану системи сигнальним світлодіодом (GPIO4, D2).
//

#ifndef MICROPLASTIC_APPLED_H
#define MICROPLASTIC_APPLED_H

#include "esp_err.h"

typedef enum {
    APP_LED_SIGNAL_REJECT = 0,   /* 2 короткі спалахи: дію кнопки відхилено */
    APP_LED_SIGNAL_REBOOT,       /* часте блимання 0.5 с перед перезапуском */
} app_led_signal_t;

/** @brief Налаштовує GPIO і створює задачу індикації. */
esp_err_t app_led_start(void);

/** @brief Стан/режим змінився — негайно перерахувати шаблон. */
void app_led_kick(void);

/** @brief Короткий сигнал поверх поточного шаблону. */
void app_led_signal(app_led_signal_t sig);

#endif //MICROPLASTIC_APPLED_H
