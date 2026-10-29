

#include "main.h"
#include <stdio.h>

#include "FreeRTOSConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "appDSP/dsp_processor_v1.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "appBuffers/appMsgBuffer.h"
#include "appBuffers/appQueueRawDataBuffer.h"
#include "appHTTPserver.h"
#include "appWiFiManager.h"

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

    xTaskCreate(dsp_processing_task, "dsp_processing_task", 8192, (void*)1, 5, NULL);

    /* Context buffer Task
     * - Perform the context estimation algorithm
     * - Notify the Network Task to grab the buffer of data
     * - Notify the LocalStorage Task to grab the data
     * - Notify *?Mode task to change the program work Mode
     */
    //xTaskCreate();
    
    /* Wi-Fi Task Configuration
     * - Configure the WiFi module of the MCU
     * - Set appropriate value
     * - *?Init the ENVS file system for saving WiFi config between reboot
     * */
    //  xTaskCreatePinnedToCore();

    /* Network Task HTTP
     * - Send a data to the Python FastAPI Server
     * - The POST endpoint "/telemetry" - send the HTTP with telemetry JSON
     * - The GET endpoint "/test_data" - request test data
     *
     */
    // xTaskCreate();
}

void app() {
          system_core_init();

          /* Черги мають існувати ДО старту DSP-задачі, інакше xQueueReceive(NULL) -> abort */
          app_QueueRawDataBuffer_create(NULL);
          app_MsgBuffer_create();

          xTaskCreate(dsp_processing_task, "dsp_processing_task", 8192, (void*)1, 5, NULL);

          /* Лише реєстрація обробників; викликати ДО wifi_manager_start() */
          app_http_server_init();

          /* Wi-Fi + Captive Portal, неблокуючий старт */
          ESP_ERROR_CHECK(wifi_manager_start());
  }
