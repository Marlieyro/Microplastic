//
// Статистика роботи системи для експериментальної частини (лише лог, тег "STAT").
//
//   * Часові мітки конвеєра одного циклу (мкс, esp_timer):
//       T_acq   — отримання кадру (читання FLASH + CRC або вимірювання AS7341);
//       T_queue — очікування кадру в черзі до задачі DSP;
//       T_dsp   — чистий час dsp_process_sensor_data();
//       T_ipc   — передача результату DSP -> задача мережі/оцінки контексту;
//       T_route — відправка HTTP або запис у FLASH;
//       T_total — від початку отримання кадру до доставки/збереження результату.
//   * Раз на APP_STATS_PERIOD_MS — знімок системи: стек і завантаження CPU
//     кожної задачі, купа, заповнення IPC, Wi-Fi, батарея, FLASH, частота CPU.
//

#ifndef MICROPLASTIC_APPSTATS_H
#define MICROPLASTIC_APPSTATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"

#define STAT_TAG "STAT"

typedef struct {
    int64_t t_acq_start;
    int64_t t_acq_end;       /* кадр поставлено в чергу DSP */
    int64_t t_dsp_start;     /* кадр отримано задачею DSP */
    int64_t t_dsp_end;
    int64_t t_route_start;   /* результат отримано задачею net / ctx */
} app_cycle_meta_t;

/** @brief Створює черги часових міток (викликати до старту задач конвеєра). */
esp_err_t app_stats_init(void);

/** @brief Запускає задачу періодичного знімка системи. */
esp_err_t app_stats_start(void);

static inline int64_t app_stats_now(void)
{
    return esp_timer_get_time();
}

/* Часові мітки йдуть паралельно даним у тому ж порядку FIFO */
void app_stats_meta_raw_put(const app_cycle_meta_t *m);   /* acq -> dsp */
bool app_stats_meta_raw_take(app_cycle_meta_t *m);        /* dsp */
void app_stats_meta_res_put(const app_cycle_meta_t *m);   /* dsp -> net (звичайна буферизація) */
bool app_stats_meta_res_take(app_cycle_meta_t *m);        /* net */

/** @brief Рядок статистики завершеного циклу. route_us — тривалість відправки/запису. */
void app_stats_log_cycle(const app_cycle_meta_t *m, const char *route, int64_t route_us, size_t bytes);

/** @brief Одноразова подія завантаження (час від старту застосунку). */
void app_stats_log_boot(const char *what);

#endif //MICROPLASTIC_APPSTATS_H
