//
// Created by user on 27.10.26.
//

#ifndef MICROPLASTIC_APPMODES_H
#define MICROPLASTIC_APPMODES_H

#include <stdbool.h>

typedef enum {
    NORMAL_MODE = 0,
    CONTEXT_BUFFERING_MODE = 1,
    LOW_ENERGY_MODE = 2,
    TEST_MODE = 3,
    CONTINUE_MODE = 4,
    LOAD_OFF_LOCAL_STORAGE_MODE = 5,
    CONFIGURATION_MODE = 6,     /* Captive Portal (налаштування Wi-Fi) */
    OTA_MODE = 7,               /* очікування/прийом оновлення прошивки */
    APP_MODES_COUNT
} applicationModes_t;

void setApplicationMode(applicationModes_t mode);
applicationModes_t getApplicationMode();

/** @brief Назва режиму для JSON ("normal", "context_buffering", ...). */
const char *appModes_toName(applicationModes_t mode);

/** @brief Перетворення назви режиму з JSON у значення enum. */
bool appModes_fromName(const char *name, applicationModes_t *out);

#endif //MICROPLASTIC_APPMODES_H
