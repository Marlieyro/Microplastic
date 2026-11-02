//
// Задача збору сирих даних.
//
// APP_DATA_SOURCE_SIM = 1 (робочий варіант): найстаріший кадр симулятора
//   читається з FLASH (rawstore) БЕЗ видалення. Видаляє його задача керування
//   лише тоді, коли результат обробки надіслано на сервер або збережено у FLASH.
// APP_DATA_SOURCE_SIM = 0: кадр вимірюється фізичним сенсором AS7341
//   (appSensorStart_Work). Фізичного пристрою немає, тому ця гілка не
//   виконується; її код компілюється й наведений для повноти системи.
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"

#include "appConfig.h"
#include "appAcq.h"
#include "appControl.h"
#include "appFlashStore.h"
#include "appQueueRawDataBuffer.h"
#include "appSensor_Sensor.h"
#include "dsp_processor_v1.h"

static const char *TAG = "ACQ";

static TaskHandle_t  s_task = NULL;
static volatile bool s_abort = false;
static SensorData    s_frame;               /* статичний: 1932 Б не на стеку задачі */

static void acquire_from_store(void)
{
    size_t len = 0;
    const esp_err_t err = app_store_peek(&g_store_raw, &s_frame, sizeof(s_frame), &len);
    if (err == ESP_ERR_NOT_FOUND) {
        app_ctrl_notify_acq_failed("no_data");
        return;
    }
    if (err != ESP_OK || len != sizeof(SensorData)) {
        ESP_LOGE(TAG, "Некоректний запис у rawstore (%s, %u Б) — видалено", esp_err_to_name(err), (unsigned)len);
        app_store_pop(&g_store_raw);
        app_ctrl_notify_acq_failed("raw_record");
        return;
    }
    if (s_abort) {
        return;
    }
    if (xQueueSend(getQueueRawDataBuffer(), &s_frame, pdMS_TO_TICKS(1000)) != pdTRUE) {
        app_ctrl_notify_acq_failed("raw_queue_full");
        return;
    }
    ESP_LOGI(TAG, "Кадр симулятора з FLASH передано на обробку DSP");
}

/* Не викликається при APP_DATA_SOURCE_SIM = 1 (фізичного сенсора немає) */
static void acquire_from_sensor(void)
{
    as7341_handle_t dev = appSensor_GetHandle();
    if (dev == NULL) {
        app_ctrl_notify_acq_failed("sensor_not_ready");
        return;
    }
    const esp_err_t err = appSensorStart_Work(dev, &s_abort);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Вимірювання перервано");
    } else if (err != ESP_OK) {
        app_ctrl_notify_acq_failed("sensor");
    }
}

static void acq_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (APP_DATA_SOURCE_SIM) {
            acquire_from_store();
        } else {
            acquire_from_sensor();
        }
    }
}

esp_err_t app_acq_start(void)
{
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(acq_task, "acq", APP_TASK_ACQ_STACK, NULL,
                                                APP_TASK_ACQ_PRIO, &s_task, APP_CORE_PROCESSING) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Не вдалося створити задачу");
    return ESP_OK;
}

void app_acq_trigger(void)
{
    s_abort = false;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void app_acq_abort(void)
{
    s_abort = true;
}
