//
// Created by user on 27.10.26.
//

#include "appModes.h"

/* Режим роботи всієї комп'юетрної системи.
 * Глобальна змінна на всю систему по якій можна дізнатись поточний режим роботи
 */
applicationModes_t currentMode;

void setApplicationMode(applicationModes_t mode) {
    currentMode = mode;
}

applicationModes_t getApplicationMode() {
    return currentMode;
}

// TODO Потрібно додати чергу режимів роботи
// тобто коли ми хочемо зміити режим роботи, або з мережі
// або в прогармі ми маємо закинути цей режим роботи в чергу Que
// потім цю чергу вичитує спецілаьна таска яка займаєтсья режимами роботи і блокуєтсья на черзі з режимимами роботи
// 