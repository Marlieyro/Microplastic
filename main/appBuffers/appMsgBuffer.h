//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPMSGBUFFER_H
#define MICROPLASTIC_APPMSGBUFFER_H

#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

void app_MsgBuffer_create();
MessageBufferHandle_t getAppMsgBuffer();
void app_streamMsg_destroy();

#endif //MICROPLASTIC_APPMSGBUFFER_H
