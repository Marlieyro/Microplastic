//
// Created by user on 27.10.26.
//
// Мережева задача "net" — ЄДИНИЙ власник HTTP-клієнта (esp_http_client не
// потокобезпечний). Відповідає за:
//   * POST /api/result  — результат DspResult (JSON + сирі дані в Base64);
//   * збереження результату у FLASH, якщо сервер недоступний або режим
//     CONTEXT_BUFFERING;
//   * вивантаження FLASH у режимі LOAD_OFF_LOCAL_STORAGE;
//   * POST /api/hello   — реєстрація пристрою + синхронізація часу;
//   * POST /api/event   — повідомлення про зміну стану.
//

#ifndef MICROPLASTIC_APPHTTPCLIENT_H
#define MICROPLASTIC_APPHTTPCLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Біти запитів до мережевої задачі (app_net_request) */
#define APP_NET_REQ_HELLO    (1u << 0)
#define APP_NET_REQ_EVENT    (1u << 1)
#define APP_NET_REQ_UPLOAD   (1u << 2)

/** @brief Ініціалізує HTTP-клієнт і створює задачу net. */
esp_err_t app_net_start(void);

/** @brief Асинхронний запит до задачі net (комбінація APP_NET_REQ_*). */
void app_net_request(uint32_t req_bits);

/** @brief Оновлює IP-адресу сервера (якщо змінилась — повторний /api/hello). */
void app_net_set_server_host(const char *ip);

/** @brief Поточна IP-адреса сервера. */
void app_net_get_server_host(char *out, size_t cap);

#endif //MICROPLASTIC_APPHTTPCLIENT_H
