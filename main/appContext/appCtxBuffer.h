//
// Контекстно-залежна мультибуферизація (режим context_buffering).
//
//   DSP ──вказівник──► q_eval ──► задача "ctx" (оцінка контексту, бали)
//                                   ├─ N < 10        ──► q_store ──► задача "store" ──► FLASH
//                                   ├─ 10 <= N < 25  ──► q_net (без сирих даних, ~600 Б)
//                                   └─ N >= 25       ──► q_net (повний результат)
//   q_net ──► задача "net" ──(не вдалося)──► q_store
//   після доставки/запису буфер повертається в q_free
//
// Пул з APP_CTX_POOL_N статичних буферів DspResult; між задачами передаються
// лише ВКАЗІВНИКИ (4 Б) — без копіювання 2 КБ даних (zero-copy).
//

#ifndef MICROPLASTIC_APPCTXBUFFER_H
#define MICROPLASTIC_APPCTXBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "dsp_processor_v1.h"
#include "appStats.h"

typedef enum {
    APP_CTX_LOCAL = 0,      /* зберегти у FLASH */
    APP_CTX_COMPACT,        /* надіслати без сирих даних */
    APP_CTX_FULL,           /* надіслати повністю */
} app_ctx_decision_t;

/** @brief Створює пул, черги і задачі ctx/store. */
esp_err_t app_ctx_start(void);

uint32_t app_ctx_pool_size(void);
uint32_t app_ctx_free_count(void);
bool     app_ctx_idle(void);                  /* усі буфери вільні */

/* ---- DSP ---- */
DspResult *app_ctx_acquire(TickType_t wait);
void       app_ctx_submit(DspResult *buf, const app_cycle_meta_t *meta);

/* ---- задача net ---- */
bool app_ctx_net_take(DspResult **buf, app_ctx_decision_t *decision, int *score);
void app_ctx_net_done(DspResult *buf, bool sent, int64_t route_us, size_t bytes);

const char *app_ctx_decision_name(app_ctx_decision_t d);

#endif //MICROPLASTIC_APPCTXBUFFER_H
