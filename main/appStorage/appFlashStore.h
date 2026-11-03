//
// Кільцева FIFO-черга записів у сирому розділі FLASH (esp_partition).
//
// Використовується двома екземплярами:
//   g_store_raw — сирі кадри SensorData, отримані з сервера (POST /api/raw);
//   g_store_res — оброблені результати DspResult, які не вдалося надіслати
//                 (або режим CONTEXT_BUFFERING).
//

#ifndef MICROPLASTIC_APPFLASHSTORE_H
#define MICROPLASTIC_APPFLASHSTORE_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_STORE_SLOT_SIZE     2048u   /* байт; 2 слоти в секторі 4 КБ */
#define APP_STORE_HDR_SIZE      16u
#define APP_STORE_MAX_PAYLOAD   (APP_STORE_SLOT_SIZE - APP_STORE_HDR_SIZE)

typedef struct {
    const char            *label;
    const esp_partition_t *part;
    SemaphoreHandle_t      lock;
    uint32_t               slots;            /* кількість слотів у розділі */
    uint32_t               slots_per_sector;
    uint32_t               write_slot;       /* наступний слот для запису */
    uint32_t               read_slot;        /* найстаріший дійсний запис */
    uint32_t               count;            /* кількість дійсних записів */
    uint32_t               next_seq;         /* порядковий номер наступного запису */
} app_store_t;

extern app_store_t g_store_raw;
extern app_store_t g_store_res;

/** @brief Ініціалізує обидва сховища (сканування заголовків, відновлення після збою). */
esp_err_t app_storage_init(void);

/** @brief Додає запис у кінець черги. ESP_ERR_NO_MEM — сховище заповнене. */
esp_err_t app_store_push(app_store_t *s, const void *data, size_t len);

/** @brief Читає найстаріший запис без видалення. ESP_ERR_NOT_FOUND — черга порожня. */
esp_err_t app_store_peek(app_store_t *s, void *buf, size_t cap, size_t *out_len);

/** @brief Читає n-й за віком запис (0 — найстаріший) без видалення.
 *         Використовується, коли кілька кадрів одночасно перебувають в обробці. */
esp_err_t app_store_peek_nth(app_store_t *s, uint32_t n, void *buf, size_t cap, size_t *out_len);

/** @brief Видаляє найстаріший запис (позначає спожитим). */
esp_err_t app_store_pop(app_store_t *s);

/** @brief Кількість записів у черзі. */
uint32_t app_store_count(app_store_t *s);

#endif //MICROPLASTIC_APPFLASHSTORE_H
