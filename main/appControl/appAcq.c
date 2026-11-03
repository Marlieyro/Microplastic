//
// Задача збору сирих даних.
//
// APP_DATA_SOURCE_SIM = 1 (робочий варіант): кадр симулятора читається з FLASH
//   (rawstore) БЕЗ видалення. Видаляє його задача керування лише тоді, коли
//   результат обробки надіслано на сервер або збережено у FLASH.
// APP_DATA_SOURCE_SIM = 0: кадр вимірюється фізичним сенсором AS7341
//   (appSensorStart_Work). Фізичного пристрою немає, тому ця гілка не
//   виконується; її код компілюється й наведений для повноти системи.
//
// Синхронізація: двійковий семафор (а не task notification індексу 0, який
// зайнятий внутрішньо stream/message buffer-ами FreeRTOS).
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "appConfig.h"
#include "appAcq.h"
#include "appControl.h"
#include "appFlashStore.h"
#include "appQueueRawDataBuffer.h"
#include "appSensor_Sensor.h"
#include "appStats.h"
#include "dsp_processor_v1.h"

static const char *TAG = "ACQ";

static SemaphoreHandle_t s_trigger = NULL;
static volatile bool     s_abort = false;
static volatile uint32_t s_raw_index = 0;
static SensorData        s_frame;           /* статичний: 1932 Б не на стеку задачі */

static void acquire_from_store(app_cycle_meta_t *meta)
{
    size_t len = 0;
    const esp_err_t err = app_store_peek_nth(&g_store_raw, s_raw_index, &s_frame, sizeof(s_frame), &len);
    if (err == ESP_ERR_NOT_FOUND) {
        app_ctrl_notify_acq_failed("no_data");
        return;
    }
    if (err != ESP_OK || len != sizeof(SensorData)) {
        ESP_LOGE(TAG, "Некоректний запис у rawstore (%s, %u Б)", esp_err_to_name(err), (unsigned)len);
        app_ctrl_notify_acq_failed("raw_record");
        return;
    }
    if (s_abort) {
        app_ctrl_notify_acq_aborted();
        return;
    }
    meta->t_acq_end = app_stats_now();
    if (xQueueSend(getQueueRawDataBuffer(), &s_frame, pdMS_TO_TICKS(1000)) != pdTRUE) {
        app_ctrl_notify_acq_failed("raw_queue_full");
        return;
    }
    app_stats_meta_raw_put(meta);
    ESP_LOGI(TAG, "Кадр симулятора [%u] з FLASH передано на обробку DSP", (unsigned)s_raw_index);
}

/* Не викликається при APP_DATA_SOURCE_SIM = 1 (фізичного сенсора немає) */
static void acquire_from_sensor(app_cycle_meta_t *meta)
{
    as7341_handle_t dev = appSensor_GetHandle();
    if (dev == NULL) {
        app_ctrl_notify_acq_failed("sensor_not_ready");
        return;
    }
    const esp_err_t err = appSensorStart_Work(dev, &s_abort);
    if (err == ESP_OK) {
        meta->t_acq_end = app_stats_now();
        app_stats_meta_raw_put(meta);
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Вимірювання перервано");
        app_ctrl_notify_acq_aborted();
    } else {
        app_ctrl_notify_acq_failed("sensor");
    }
}

static void acq_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_trigger, portMAX_DELAY);
        app_cycle_meta_t meta = { 0 };
        meta.t_acq_start = app_stats_now();
        if (APP_DATA_SOURCE_SIM) {
            acquire_from_store(&meta);
        } else {
            acquire_from_sensor(&meta);
        }
    }
}

esp_err_t app_acq_start(void)
{
    s_trigger = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_trigger != NULL, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(acq_task, "acq", APP_TASK_ACQ_STACK, NULL,
                                                APP_TASK_ACQ_PRIO, NULL, APP_CORE_PROCESSING) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

void app_acq_trigger(uint32_t raw_index)
{
    s_abort = false;
    s_raw_index = raw_index;
    if (s_trigger != NULL) {
        xSemaphoreGive(s_trigger);
    }
}

void app_acq_abort(void)
{
    s_abort = true;
}
