//
// Задача збору сирих даних (acquisition).
// За сигналом задачі керування отримує один повний кадр SensorData
// (32 x 10 для фаз DARK / IR / BLUE) і передає його в чергу DSP.
//

#ifndef MICROPLASTIC_APPACQ_H
#define MICROPLASTIC_APPACQ_H

#include "esp_err.h"

/** @brief Створює задачу збору даних. */
esp_err_t app_acq_start(void);

/** @brief Запускає один цикл збору (викликає задача керування). */
void app_acq_trigger(void);

/** @brief Просить перервати поточний цикл (stop_work / ERROR). */
void app_acq_abort(void);

#endif //MICROPLASTIC_APPACQ_H
