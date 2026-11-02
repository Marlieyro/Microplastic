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
#include "nvs_flash.h"

#include "appConfig.h"
#include "appDSP/dsp_processor_v1.h"
#include "appBuffers/appMsgBuffer.h"
#include "appBuffers/appQueueRawDataBuffer.h"
#include "appModes.h"
#include "appFlashStore.h"
#include "appControl.h"
#include "appAcq.h"
#include "appLed.h"
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
    /* 1. NVS, TCP/IP-стек, системний цикл подій */
    system_core_init();

    /* 2. Черга сирих даних і message buffer — ДО створення задач, що їх використовують */
    app_QueueRawDataBuffer_create(NULL);
    app_MsgBuffer_create();
    if (getQueueRawDataBuffer() == NULL || getAppMsgBuffer() == NULL) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);

    /* 3. Режим за замовчуванням і безпечний стан LED збудження */
    setApplicationMode(NORMAL_MODE);
    appSensor_GpioInit();

    /* 4. Сховище у FLASH (rawstore / resstore) */
    const esp_err_t storage_err = app_storage_init();

    /* 5. Задача керування — до Wi-Fi, щоб не пропустити подію підключення */
    ESP_ERROR_CHECK(app_ctrl_start());
    if (storage_err != ESP_OK) app_ctrl_notify_fault("storage");


    /* 6. Фізичний сенсор — лише якщо дані не від симулятора (у роботі не використовується) */
    if (!APP_DATA_SOURCE_SIM) {
        appI2C_Init();
        if (appSensor_Config() != ESP_OK) app_ctrl_notify_fault("sensor");
    }

    /* 7. Робочі задачі */
    ESP_ERROR_CHECK(app_led_start());
    ESP_ERROR_CHECK(app_acq_start());
    if (xTaskCreatePinnedToCore(dsp_processing_task, "dsp", APP_TASK_DSP_STACK, NULL,
                                APP_TASK_DSP_PRIO, NULL, APP_CORE_PROCESSING) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_ERROR_CHECK(app_net_start());

    /* 8. HTTP-сервер (лише реєстрація подій) і Wi-Fi + Captive Portal */
    app_http_server_init();
    ESP_ERROR_CHECK(wifi_manager_start());

    ESP_LOGI(TAG, "Ініціалізацію завершено, джерело даних: %s",
             APP_DATA_SOURCE_SIM ? "HIL-симулятор (FLASH)" : "AS7341");
}
