//
// Created by user on 27.10.26.
//
// Captive Portal = DHCP-опція 114 + DNS-сервер + HTTP-сервер сторінки налаштування.
// Модуль лише "обслуговує" портал; рішення КОЛИ його вмикати/вимикати
// приймає менеджер Wi-Fi (appWiFiManager.c).
//

#ifndef MICROPLASTIC_APPCAPTIVEPORTAL_H
#define MICROPLASTIC_APPCAPTIVEPORTAL_H

#include "esp_err.h"
#include "esp_netif.h"

/**
 * @brief Запускає Captive Portal на інтерфейсі точки доступу.
 *        Інтерфейс AP вже має бути запущений (WIFI_EVENT_AP_START).
 * @param ap_netif netif точки доступу (WIFI_AP_DEF).
 */
esp_err_t captive_portal_start(esp_netif_t *ap_netif);

/** @brief Зупиняє HTTP- та DNS-сервер порталу (звільняє порти 80 і 53). */
void captive_portal_stop(void);

#endif //MICROPLASTIC_APPCAPTIVEPORTAL_H
