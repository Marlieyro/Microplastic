//
// Created by user on 27.10.26.
//
// HTTP-сервер застосунку (порт 80), стартує після підключення до Wi-Fi.
//   POST /api/command  {"cmd":"start_work"}       -> {"ok":true,"state":"working"}
//   POST /api/mode     {"mode":"context_buffering"} -> {"ok":true,"mode":"context_buffering"}
//                      (configuration -> додатково "warning":"connection_will_be_lost")
//   GET  /api/status   -> {"state":..,"mode":..,"raw":N,"res":M,"err":..,"battery":%,"fw":".."}
//   POST /api/raw      сирі дані симулятора -> FLASH (rawstore)
//        Content-Type: text/plain               — текст (3 температури + 96 рядків [..])
//        Content-Type: application/octet-stream — бінарно, рівно sizeof(SensorData) = 1932 Б
//   POST /api/ota      файл прошивки .bin (лише в режимі ota) -> неактивний OTA-слот -> перезапуск
//

#ifndef MICROPLASTIC_APPHTTPSERVER_H
#define MICROPLASTIC_APPHTTPSERVER_H

/** @brief Реєструє запуск/зупинку сервера за подіями Wi-Fi. Викликати ДО wifi_manager_start(). */
void app_http_server_init(void);

#endif //MICROPLASTIC_APPHTTPSERVER_H
