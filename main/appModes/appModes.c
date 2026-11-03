//
// Created by user on 27.10.26.
//

#include <string.h>
#include "appModes.h"

/* Режим роботи всієї комп'ютерної системи.
 * Змінюється ЛИШЕ задачею керування (appControl.c); інші задачі тільки читають.
 * volatile + 32-бітний enum: читання/запис атомарні на Xtensa LX7.
 */
static volatile applicationModes_t currentMode = NORMAL_MODE;

static const char *const s_mode_names[APP_MODES_COUNT] = {
    [NORMAL_MODE]                 = "normal",
    [CONTEXT_BUFFERING_MODE]      = "context_buffering",
    [LOW_ENERGY_MODE]             = "low_energy",
    [TEST_MODE]                   = "test",
    [CONTINUE_MODE]               = "continue",
    [LOAD_OFF_LOCAL_STORAGE_MODE] = "load_off_local_storage",
    [CONFIGURATION_MODE]          = "configuration",
    [OTA_MODE]                    = "ota",
};

void setApplicationMode(applicationModes_t mode) {
    if (mode < APP_MODES_COUNT) {
        currentMode = mode;
    }
}

applicationModes_t getApplicationMode() {
    return currentMode;
}

const char *appModes_toName(applicationModes_t mode) {
    return (mode < APP_MODES_COUNT) ? s_mode_names[mode] : "unknown";
}

bool appModes_fromName(const char *name, applicationModes_t *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < APP_MODES_COUNT; i++) {
        if (strcmp(name, s_mode_names[i]) == 0) {
            *out = (applicationModes_t)i;
            return true;
        }
    }
    return false;
}
