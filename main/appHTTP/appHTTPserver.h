//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPHTTPSERVER_H
#define MICROPLASTIC_APPHTTPSERVER_H

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err);
void app_http_server_init(void);

#endif //MICROPLASTIC_APPHTTPSERVER_H
