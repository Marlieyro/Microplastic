//
// Created by user on 27.10.26.
//

#include "appMsgBuffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

static const char *TAG = "MSG_BUFFER";

// TODO помніять це елементи чи ні
#define BUFFER_SIZE 1400
// #define BUFFER_TRIGGER_LEVEL 1300 // Bytes

static MessageBufferHandle_t xMsgBuffer = NULL;

void app_MsgBuffer_create() {
    xMsgBuffer = xMessageBufferCreate(BUFFER_SIZE);
    if( xMsgBuffer == NULL ) {
        ESP_LOGI(TAG, "xMessageBufferCreate failed");
    } else {
        ESP_LOGI(TAG, "xMessageBufferCreate succeeded");
    }
}

MessageBufferHandle_t getAppMsgBuffer() {
    return xMsgBuffer;
}

void app_streamMsg_destroy() {
    vMessageBufferDelete(xMsgBuffer);
    xMsgBuffer = NULL;
    ESP_LOGI(TAG, "xMsgBufferDelete succeeded");
}
