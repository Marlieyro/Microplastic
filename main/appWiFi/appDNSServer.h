//
// Created by user on 27.10.26.
//
// Мінімальний DNS-сервер для Captive Portal.
// На будь-який запит типу A (IPv4) відповідає однією адресою — IP точки
// доступу ESP32. На AAAA (IPv6) та інші типи відповідає "немає записів"
// (NOERROR, 0 відповідей), щоб клієнт одразу перейшов на IPv4.
//

#ifndef MICROPLASTIC_APPDNSSERVER_H
#define MICROPLASTIC_APPDNSSERVER_H

#include "esp_err.h"
#include "esp_netif.h"

/**
 * @brief Запускає DNS-сервер (UDP, порт 53) в окремій задачі.
 * @param answer_ip IP-адреса, яку сервер повертатиме на всі A-запити.
 */
esp_err_t dns_server_start(esp_ip4_addr_t answer_ip);

/**
 * @brief Коректно зупиняє DNS-сервер: задача сама закриває сокет
 *        і завершується; функція чекає на це завершення.
 */
void dns_server_stop(void);

#endif //MICROPLASTIC_APPDNSSERVER_H
