//
// Мінімальний JSON-парсер (плоский об'єкт верхнього рівня) та Base64.
//
// Парсер не будує дерево: він послідовно проходить пари "ключ": значення
// об'єкта верхнього рівня й зупиняється на потрібному ключі. Вкладені
// об'єкти/масиви коректно пропускаються, але їхній вміст не аналізується.
//

#include <string.h>
#include <stdlib.h>
#include "appJson.h"

#define JSON_KEY_MAX 32

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

/*
 * Розбирає рядок у лапках. out == NULL — лише пропуск.
 * Повертає false тільки при синтаксичній помилці; *fits = false, якщо рядок
 * не вмістився в out (тоді out містить обрізаний рядок).
 */
static bool parse_string(cursor_t *c, char *out, size_t cap, bool *fits)
{
    size_t n = 0;
    if (fits != NULL) {
        *fits = true;
    }
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (out != NULL && cap > 0) {
                out[n < cap ? n : cap - 1] = '\0';
            }
            return true;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                return false;
            }
            const char e = *c->p++;
            switch (e) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u':                       /* \uXXXX у наших схемах не використовується */
                if (c->end - c->p < 4) {
                    return false;
                }
                c->p += 4;
                ch = '?';
                break;
            default:  ch = e; break;        /* \" \\ \/ */
            }
        }
        if (out != NULL) {
            if (n + 1 < cap) {
                out[n++] = ch;
            } else if (fits != NULL) {
                *fits = false;
            }
        }
    }
    return false;
}

/* Пропускає будь-яке значення: рядок, число, true/false/null, об'єкт, масив */
static bool skip_value(cursor_t *c)
{
    skip_ws(c);
    if (c->p >= c->end) {
        return false;
    }
    if (*c->p == '"') {
        return parse_string(c, NULL, 0, NULL);
    }
    if (*c->p == '{' || *c->p == '[') {
        int depth = 0;
        bool in_str = false;
        while (c->p < c->end) {
            const char ch = *c->p++;
            if (in_str) {
                if (ch == '\\' && c->p < c->end) {
                    c->p++;
                } else if (ch == '"') {
                    in_str = false;
                }
                continue;
            }
            if (ch == '"') {
                in_str = true;
            } else if (ch == '{' || ch == '[') {
                depth++;
            } else if (ch == '}' || ch == ']') {
                if (--depth == 0) {
                    return true;
                }
            }
        }
        return false;
    }
    const char *start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
           *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r') {
        c->p++;
    }
    return c->p > start;
}

/* Встановлює курсор val на початок значення ключа key */
static bool find_key(const char *json, const char *key, cursor_t *val)
{
    if (json == NULL || key == NULL) {
        return false;
    }
    cursor_t c = { json, json + strlen(json) };
    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') {
        return false;
    }
    c.p++;

    char name[JSON_KEY_MAX];
    for (;;) {
        skip_ws(&c);
        if (c.p >= c.end || *c.p == '}') {
            return false;
        }
        bool fits = true;
        if (!parse_string(&c, name, sizeof(name), &fits)) {
            return false;
        }
        skip_ws(&c);
        if (c.p >= c.end || *c.p != ':') {
            return false;
        }
        c.p++;
        skip_ws(&c);
        if (fits && strcmp(name, key) == 0) {
            *val = c;
            return true;
        }
        if (!skip_value(&c)) {
            return false;
        }
        skip_ws(&c);
        if (c.p < c.end && *c.p == ',') {
            c.p++;
            continue;
        }
        return false;
    }
}

bool app_json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    cursor_t v;
    bool fits = true;
    if (out == NULL || cap == 0 || !find_key(json, key, &v)) {
        return false;
    }
    return parse_string(&v, out, cap, &fits) && fits;
}

bool app_json_get_number(const char *json, const char *key, double *out)
{
    cursor_t v;
    if (out == NULL || !find_key(json, key, &v)) {
        return false;
    }
    char *endp = NULL;
    const double d = strtod(v.p, &endp);   /* json NUL-термінований */
    if (endp == v.p) {
        return false;
    }
    *out = d;
    return true;
}

size_t app_b64_encode(const uint8_t *src, size_t len, char *dst, size_t cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = 4 * ((len + 2) / 3);
    if (dst == NULL || cap < need + 1) {
        return 0;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 2 < len) {
        const uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = tbl[(v >> 18) & 0x3F];
        dst[o++] = tbl[(v >> 12) & 0x3F];
        dst[o++] = tbl[(v >> 6) & 0x3F];
        dst[o++] = tbl[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)src[i + 1] << 8;
        }
        dst[o++] = tbl[(v >> 18) & 0x3F];
        dst[o++] = tbl[(v >> 12) & 0x3F];
        dst[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        dst[o++] = '=';
    }
    dst[o] = '\0';
    return o;
}
