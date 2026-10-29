//
// Created by user on 27.10.26.
//


#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "dsp_processor_v1.h"
#include "appQueueRawDataBuffer.h"

static const char *TAG = "MSG_BUFFER";

#define BUFFER_SIZE 10 // Items
// #define BUFFER_TRIGGER_LEVEL 1300 // Bytes

static QueueHandle_t xQueueRawDataBuffer = NULL;

void app_QueueRawDataBuffer_create(const SensorData* rawDataBuffer) {
    xQueueRawDataBuffer = xQueueCreate(BUFFER_SIZE, sizeof(SensorData));
    if( xQueueRawDataBuffer == NULL ) {
        ESP_LOGI(TAG, "xQueueRawDataBuffer failed");
    } else {
        ESP_LOGI(TAG, "xQueueRawDataBuffer succeeded");
    }
}

QueueHandle_t getQueueRawDataBuffer() {
    return xQueueRawDataBuffer;
}

void app_QueueRawDataBuffer_destroy() {
    vQueueUnregisterQueue(xQueueRawDataBuffer);
    xQueueRawDataBuffer = NULL;
    ESP_LOGI(TAG, "xQueueRawDataBuffer Delete succeeded");
}