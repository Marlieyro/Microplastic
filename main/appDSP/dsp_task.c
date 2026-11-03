//
// Задача FreeRTOS обробки DSP (перенесена з dsp_processor_v1.c).
// Алгоритм не змінено: викликається dsp_process_sensor_data() з dsp_processor_v1.c.
//
// Два шляхи передачі результату (обирається режимом на момент обробки):
//   * звичайна буферизація: результат копіюється в message buffer -> задача net;
//   * context_buffering: результат пишеться ОДРАЗУ у вільний буфер пулу
//     (zero-copy), вказівник передається задачі оцінки контексту.
//

#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"

#include "appConfig.h"
#include "dsp_processor_v1.h"
#include "appMsgBuffer.h"
#include "appQueueRawDataBuffer.h"
#include "appControl.h"
#include "appModes.h"
#include "appCtxBuffer.h"
#include "appHTTPclient.h"
#include "appStats.h"

static const char *TAG = "DSP_TASK";

/* Статичні буфери: 1932 + 1997 Б не займають стек задачі */
static SensorData s_input;
static DspResult  s_result;

static void set_timestamp(DspResult *r)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((long long)tv.tv_sec > APP_TIME_VALID_AFTER_S) {
        r->timestamp = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
        r->timestamp_valid = true;
    } else {
        r->timestamp = 0;
        r->timestamp_valid = false;
    }
}

void dsp_processing_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "DSP task запущено; очікування SensorData у FreeRTOS queue.");

    for (;;) {
        if (xQueueReceive(getQueueRawDataBuffer(), &s_input, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        app_cycle_meta_t meta = { 0 };
        app_stats_meta_raw_take(&meta);

        const bool ctx = (getApplicationMode() == CONTEXT_BUFFERING_MODE);
        DspResult *out = &s_result;
        if (ctx) {
            out = app_ctx_acquire(pdMS_TO_TICKS(2000));
            if (out == NULL) {
                ESP_LOGE(TAG, "Пул мультибуферизації вичерпано — результат не сформовано");
                app_ctrl_notify_result_done(false, "pool_full");
                continue;
            }
        }

        meta.t_dsp_start = app_stats_now();
        const esp_err_t err = dsp_process_sensor_data(&s_input, out);
        meta.t_dsp_end = app_stats_now();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Обробка завершилась помилкою: %s (status=%d).",
                     esp_err_to_name(err), (int)out->status);
        }

        /* Мітка часу ставиться ПІСЛЯ обробки, бо dsp_process_sensor_data() обнуляє result */
        set_timestamp(out);

        if (ctx) {
            app_ctx_submit(out, &meta);
            continue;
        }

        /*
         * Не відкидаємо вимірювання при помилці DSP: у буфер передається
         * запис зі статусом помилки та копією сирих даних.
         * xMessageBufferSend() повертає КІЛЬКІСТЬ записаних байтів.
         */
        const size_t sent = xMessageBufferSend(getAppMsgBuffer(), &s_result, sizeof(DspResult),
                                               pdMS_TO_TICKS(5000));
        if (sent != sizeof(DspResult)) {
            ESP_LOGE(TAG, "Не вдалося передати DspResult у message buffer.");
            app_ctrl_notify_result_done(false, "msg_buffer");
            continue;
        }
        app_stats_meta_res_put(&meta);
        app_net_request(APP_NET_REQ_RESULT);
    }
}
