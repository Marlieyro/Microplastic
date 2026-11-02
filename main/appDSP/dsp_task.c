//
// Задача FreeRTOS обробки DSP (перенесена з dsp_processor_v1.c).
// Алгоритм не змінено: викликається dsp_process_sensor_data() з dsp_processor_v1.c.
//
// Потік: черга SensorData -> dsp_process_sensor_data() -> message buffer DspResult -> задача net.
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

static const char *TAG = "DSP_TASK";

/* Статичні буфери: 1932 + 1997 Б не займають стек задачі */
static SensorData s_input;
static DspResult  s_result;

void dsp_processing_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "DSP task запущено; очікування SensorData у FreeRTOS queue.");

    for (;;) {
        if (xQueueReceive(getQueueRawDataBuffer(), &s_input, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const esp_err_t err = dsp_process_sensor_data(&s_input, &s_result);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Обробка завершилась помилкою: %s (status=%d).",
                     esp_err_to_name(err), (int)s_result.status);
        }

        /* Мітка часу ставиться ПІСЛЯ обробки, бо dsp_process_sensor_data() обнуляє result */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if ((long long)tv.tv_sec > APP_TIME_VALID_AFTER_S) {
            s_result.timestamp = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
            s_result.timestamp_valid = true;
        } else {
            s_result.timestamp = 0;
            s_result.timestamp_valid = false;
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
        }
    }
}
