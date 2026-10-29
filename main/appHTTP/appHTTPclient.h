//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPHTTPCLIENT_H
#define MICROPLASTIC_APPHTTPCLIENT_H

#include "dsp_processor_v1.h"
#include "appBuffers/appMsgBuffer.h"

void app_http_client_init(void);
void app_http_client_post_binary(const DspResult* result_data);
void app_http_client_deinit(void);
void vTask_HTTP_POST_request(void *arg);

#endif //MICROPLASTIC_APPHTTPCLIENT_H
