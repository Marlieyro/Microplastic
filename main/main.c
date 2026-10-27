

#include "main.h"
#include <stdio.h>

#include "FreeRTOSConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "appDSP/dsp_processor_v1.h"

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
