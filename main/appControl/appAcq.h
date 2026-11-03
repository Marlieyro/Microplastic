//
// Задача збору сирих даних (acquisition).
// За сигналом задачі керування отримує один повний кадр SensorData
// (32 x 10 для фаз DARK / IR / BLUE) і передає його в чергу DSP.
//

#ifndef MICROPLASTIC_APPACQ_H
#define MICROPLASTIC_APPACQ_H

#include <stdint.h>
#include "esp_err.h"

/** @brief Створює задачу збору даних. */
esp_err_t app_acq_start(void);

/** @brief Запускає один цикл збору. raw_index — номер кадру в rawstore (0 — найстаріший),
 *         потрібен, коли попередні кадри ще в обробці (контекстна мультибуферизація). */
void app_acq_trigger(uint32_t raw_index);

/** @brief Просить перервати поточний цикл (stop_work / ERROR). */
void app_acq_abort(void);

#endif //MICROPLASTIC_APPACQ_H
