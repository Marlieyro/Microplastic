//
// Мінімальний JSON-парсер для плоских об'єктів {"key":"value", "n":123}
// та кодер Base64 (для вставки бінарних сирих даних у JSON).
//

#ifndef MICROPLASTIC_APPJSON_H
#define MICROPLASTIC_APPJSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Знаходить у плоскому JSON-об'єкті рядкове значення ключа.
 * @param json NUL-термінований текст JSON.
 * @return true — ключ знайдено, значення є рядком і вміщується в out.
 */
bool app_json_get_str(const char *json, const char *key, char *out, size_t cap);

/**
 * @brief Знаходить у плоскому JSON-об'єкті числове значення ключа.
 * @return true — ключ знайдено і значення є числом.
 */
bool app_json_get_number(const char *json, const char *key, double *out);

/**
 * @brief Base64 (RFC 4648) кодування.
 * @return кількість записаних символів (без NUL) або 0, якщо cap замалий.
 */
size_t app_b64_encode(const uint8_t *src, size_t len, char *dst, size_t cap);

#endif //MICROPLASTIC_APPJSON_H
