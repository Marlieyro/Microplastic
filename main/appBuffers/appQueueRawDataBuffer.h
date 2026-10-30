//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPQUEUERAWDATABUFFER_H
#define MICROPLASTIC_APPQUEUERAWDATABUFFER_H
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "dsp_processor_v1.h"

void app_QueueRawDataBuffer_create(const SensorData* rawDataBuffer);
QueueHandle_t getQueueRawDataBuffer();
void app_QueueRawDataBuffer_destroy();
#endif //MICROPLASTIC_APPQUEUERAWDATABUFFER_H
