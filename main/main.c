//
// Точка входу прошивки: ініціалізація сервісів і створення задач FreeRTOS.
// Подальша логіка роботи — у задачі керування app_ctrl (appControl.c).
//

#include "main.h"
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"

#include "appConfig.h"
#include "appDSP/dsp_processor_v1.h"
#include "appBuffers/appMsgBuffer.h"
#include "appBuffers/appQueueRawDataBuffer.h"
#include "appModes.h"
#include "appFlashStore.h"
#include "appControl.h"
#include "appAcq.h"
#include "appCtxBuffer.h"
#include "appLed.h"
#include "appButton.h"
#include "appBattery.h"
#include "appPower.h"
#include "appStats.h"
#include "appHTTPclient.h"
#include "appHTTPserver.h"
#include "appWiFiManager.h"
#include "appSensor_Sensor.h"
#include "appSensor_I2CConfig.h"

static const char *TAG = "MAIN";

/* Базові системні сервіси — ініціалізуються ОДИН раз для всієї програми */
static void system_core_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

void app_main(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Прошивка %s, розділ «%s»", esp_app_get_description()->version,
             running ? running->label : "?");

    /* 1. NVS, TCP/IP-стек, системний цикл подій */
    system_core_init();

    /* 2. IPC — ДО створення задач, що їх використовують */
    app_QueueRawDataBuffer_create(NULL);
    app_MsgBuffer_create();
    if (getQueueRawDataBuffer() == NULL || getAppMsgBuffer() == NULL) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(app_stats_init());

    /* 3. Режим за замовчуванням і безпечний стан LED збудження */
    setApplicationMode(NORMAL_MODE);
    appSensor_GpioInit();

    /* 4. Сховище у FLASH (rawstore / resstore) */
    const esp_err_t storage_err = app_storage_init();

    /* 5. Задача керування — до Wi-Fi, щоб не пропустити події підключення/порталу */
    ESP_ERROR_CHECK(app_ctrl_start());
    if (storage_err != ESP_OK) app_ctrl_notify_fault("storage");

    /* 6. Шина I2C (спільна: MAX17043 + AS7341). Сенсор — лише без симулятора */
    appI2C_Init();
    if (!APP_DATA_SOURCE_SIM) {
        if (appSensor_Config() != ESP_OK) app_ctrl_notify_fault("sensor");
    }
    app_battery_start();                 /* відсутність датчика не є помилкою */

    /* 7. Індикація, кнопка, пробудження з light sleep */
    ESP_ERROR_CHECK(app_led_start());
    ESP_ERROR_CHECK(app_button_start());
    app_power_init();

    /* 8. Конвеєр обробки */
    ESP_ERROR_CHECK(app_ctx_start());
    ESP_ERROR_CHECK(app_acq_start());
    if (xTaskCreatePinnedToCore(dsp_processing_task, "dsp", APP_TASK_DSP_STACK, NULL,
                                APP_TASK_DSP_PRIO, NULL, APP_CORE_PROCESSING) != pdPASS) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(app_net_start());
    ESP_ERROR_CHECK(app_stats_start());

    /* 9. HTTP-сервер (лише реєстрація подій) і Wi-Fi + Captive Portal */
    app_http_server_init();
    ESP_ERROR_CHECK(wifi_manager_start());

    ESP_LOGI(TAG, "Ініціалізацію завершено, джерело даних: %s",
             APP_DATA_SOURCE_SIM ? "HIL-симулятор (FLASH)" : "AS7341");
    app_stats_log_boot("ініціалізацію завершено");
}
