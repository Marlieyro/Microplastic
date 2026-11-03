//
// Кільцева FIFO-черга записів у FLASH.
//
// Фізика NOR-flash, на якій побудовано алгоритм:
//   * стирання — лише цілими секторами по 4 КБ (усі біти -> 1, байти 0xFF);
//   * програмування може змінювати біти лише 1 -> 0, тому в уже записане поле
//     можна дописати значення з меншою кількістю одиниць без стирання.
//
// Розмітка слота (2048 Б, два слоти на сектор):
//   [magic u32][seq u32][len u16][state u16][crc32 u32][payload ... ]
// Стан запису (поле state):
//   0xFFFF — слот стертий або запис ще не завершено;
//   0x00FF — запис дійсний (дописується ОСТАННІМ, після корисних даних);
//   0x0000 — запис спожитий («видалений»).
// Порядок запису «заголовок -> дані -> state=VALID» гарантує, що після
// раптової втрати живлення напівзаписаний слот не буде прийнятий як дійсний.
// Сектор стирається «ліниво» — коли покажчик запису входить у сектор, усі
// записи якого вже спожиті. Кільце забезпечує рівномірне зношування секторів.
//

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_crc.h"
#include "appFlashStore.h"
#include "appConfig.h"
#include "appStats.h"

#define HDR_MAGIC       0x3153504DU     /* "MPS1" */
#define ST_EMPTY        0xFFFFU
#define ST_VALID        0x00FFU
#define ST_CONSUMED     0x0000U
#define STATE_OFFSET    10u             /* зміщення поля state у заголовку */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint16_t len;
    uint16_t state;
    uint32_t crc;
} slot_hdr_t;

_Static_assert(sizeof(slot_hdr_t) == APP_STORE_HDR_SIZE, "slot header size");

typedef enum {
    SLOT_ERASED = 0,
    SLOT_VALID,
    SLOT_PENDING,   /* magic є, але state = 0xFFFF — запис перервано */
    SLOT_USED,      /* спожитий або пошкоджений */
} slot_class_t;

static const char *TAG = "FLASH_STORE";

app_store_t g_store_raw = { .label = APP_STORE_RAW_LABEL };
app_store_t g_store_res = { .label = APP_STORE_RES_LABEL };

static int64_t s_last_erase_us = 0;     /* тривалість останнього стирання (для статистики) */

static size_t slot_offset(const app_store_t *s, uint32_t slot)
{
    return (size_t)slot * APP_STORE_SLOT_SIZE;
}

static esp_err_t read_hdr(const app_store_t *s, uint32_t slot, slot_hdr_t *h)
{
    return esp_partition_read(s->part, slot_offset(s, slot), h, sizeof(*h));
}

static slot_class_t classify(const slot_hdr_t *h)
{
    static const uint8_t erased[sizeof(slot_hdr_t)] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    if (memcmp(h, erased, sizeof(erased)) == 0) {
        return SLOT_ERASED;
    }
    if (h->magic == HDR_MAGIC && h->len <= APP_STORE_MAX_PAYLOAD) {
        if (h->state == ST_VALID) {
            return SLOT_VALID;
        }
        if (h->state == ST_EMPTY) {
            return SLOT_PENDING;
        }
    }
    return SLOT_USED;
}

static esp_err_t write_state(const app_store_t *s, uint32_t slot, uint16_t state)
{
    return esp_partition_write(s->part, slot_offset(s, slot) + STATE_OFFSET, &state, sizeof(state));
}

static esp_err_t store_init_one(app_store_t *s)
{
    s->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, s->label);
    ESP_RETURN_ON_FALSE(s->part != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "Розділ «%s» не знайдено (перевірте partitions.csv)", s->label);

    const uint32_t erase_size = (s->part->erase_size != 0) ? s->part->erase_size : 4096u;
    s->slots_per_sector = erase_size / APP_STORE_SLOT_SIZE;
    ESP_RETURN_ON_FALSE(s->slots_per_sector >= 1, ESP_ERR_INVALID_SIZE, TAG, "Слот більший за сектор");
    s->slots = (uint32_t)(s->part->size / erase_size) * s->slots_per_sector;

    if (s->lock == NULL) {
        s->lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s->lock != NULL, ESP_ERR_NO_MEM, TAG, "Немає пам'яті");
    }

    bool     have_any = false, have_valid = false;
    uint32_t max_seq = 0, max_slot = 0, min_seq = 0, min_slot = 0;
    s->count = 0;

    for (uint32_t i = 0; i < s->slots; i++) {
        slot_hdr_t h;
        ESP_RETURN_ON_ERROR(read_hdr(s, i, &h), TAG, "Помилка читання FLASH");
        const slot_class_t cls = classify(&h);

        if (cls == SLOT_PENDING) {
            /* Запис перервано втратою живлення — позначаємо спожитим */
            write_state(s, i, ST_CONSUMED);
        }
        if (cls != SLOT_ERASED && h.magic == HDR_MAGIC) {
            if (!have_any || h.seq > max_seq) {
                max_seq = h.seq;
                max_slot = i;
                have_any = true;
            }
        }
        if (cls == SLOT_VALID) {
            s->count++;
            if (!have_valid || h.seq < min_seq) {
                min_seq = h.seq;
                min_slot = i;
                have_valid = true;
            }
        }
    }

    s->write_slot = have_any ? (max_slot + 1) % s->slots : 0;
    s->next_seq   = have_any ? max_seq + 1 : 1;
    s->read_slot  = have_valid ? min_slot : s->write_slot;

    ESP_LOGI(TAG, "«%s»: %u КБ, слотів %u, записів %u",
             s->label, (unsigned)(s->part->size / 1024), (unsigned)s->slots, (unsigned)s->count);
    return ESP_OK;
}

esp_err_t app_storage_init(void)
{
    const esp_err_t e1 = store_init_one(&g_store_raw);
    const esp_err_t e2 = store_init_one(&g_store_res);
    return (e1 != ESP_OK) ? e1 : e2;
}

/* Готує write_slot до запису: стирає сектор при вході в нього. Викликати під lock. */
static esp_err_t prepare_write_slot(app_store_t *s)
{
    for (uint32_t guard = 0; guard <= s->slots; guard++) {
        const uint32_t slot = s->write_slot;
        slot_hdr_t h;

        if (slot % s->slots_per_sector == 0) {
            bool has_valid = false;
            bool dirty = false;
            for (uint32_t k = 0; k < s->slots_per_sector; k++) {
                ESP_RETURN_ON_ERROR(read_hdr(s, slot + k, &h), TAG, "Помилка читання FLASH");
                const slot_class_t cls = classify(&h);
                has_valid |= (cls == SLOT_VALID);
                dirty     |= (cls != SLOT_ERASED);
            }
            if (has_valid) {
                return ESP_ERR_NO_MEM;          /* кільце заповнене: попереду найстаріші записи */
            }
            if (dirty) {
                const size_t sector_size = (size_t)s->slots_per_sector * APP_STORE_SLOT_SIZE;
                const int64_t t0 = app_stats_now();
                ESP_RETURN_ON_ERROR(esp_partition_erase_range(s->part, slot_offset(s, slot), sector_size),
                                    TAG, "Помилка стирання сектора");
                s_last_erase_us = app_stats_now() - t0;
            }
            return ESP_OK;
        }

        ESP_RETURN_ON_ERROR(read_hdr(s, slot, &h), TAG, "Помилка читання FLASH");
        if (classify(&h) == SLOT_ERASED) {
            return ESP_OK;
        }
        /* Неочікувано «брудний» слот посеред сектора — переходимо до наступного сектора */
        s->write_slot = ((slot / s->slots_per_sector + 1) * s->slots_per_sector) % s->slots;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t app_store_push(app_store_t *s, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s != NULL && s->part != NULL && data != NULL, ESP_ERR_INVALID_STATE, TAG, "Сховище не ініціалізоване");
    ESP_RETURN_ON_FALSE(len > 0 && len <= APP_STORE_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE, TAG, "Задовгий запис");

    const int64_t t0 = app_stats_now();
    xSemaphoreTake(s->lock, portMAX_DELAY);
    s_last_erase_us = 0;
    esp_err_t err = prepare_write_slot(s);
    if (err == ESP_OK) {
        const uint32_t slot = s->write_slot;
        const slot_hdr_t h = {
            .magic = HDR_MAGIC,
            .seq   = s->next_seq,
            .len   = (uint16_t)len,
            .state = ST_EMPTY,
            .crc   = esp_rom_crc32_le(0, (const uint8_t *)data, (uint32_t)len),
        };
        err = esp_partition_write(s->part, slot_offset(s, slot), &h, sizeof(h));
        if (err == ESP_OK) {
            err = esp_partition_write(s->part, slot_offset(s, slot) + APP_STORE_HDR_SIZE, data, len);
        }
        if (err == ESP_OK) {
            err = write_state(s, slot, ST_VALID);
        }
        if (err == ESP_OK) {
            if (s->count == 0) {
                s->read_slot = slot;
            }
            s->count++;
            s->next_seq++;
            s->write_slot = (slot + 1) % s->slots;
        } else {
            write_state(s, slot, ST_CONSUMED);  /* не залишати напівзаписаний слот */
            s->write_slot = (slot + 1) % s->slots;
        }
    }
    const int64_t erase_us = s_last_erase_us;
    xSemaphoreGive(s->lock);

    if (err == ESP_OK) {
        ESP_LOGI(STAT_TAG, "flash write «%s»: %u Б за %.3f мс (з них стирання сектора %.3f мс)",
                 s->label, (unsigned)len, (double)(app_stats_now() - t0) / 1000.0, (double)erase_us / 1000.0);
    }
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "«%s» заповнене", s->label);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "«%s»: помилка запису: %s", s->label, esp_err_to_name(err));
    }
    return err;
}

/* Пересуває read_slot до найближчого дійсного запису. Викликати під lock. */
static bool seek_valid(app_store_t *s, slot_hdr_t *h)
{
    for (uint32_t guard = 0; guard < s->slots && s->count > 0; guard++) {
        if (read_hdr(s, s->read_slot, h) != ESP_OK) {
            return false;
        }
        if (classify(h) == SLOT_VALID) {
            return true;
        }
        s->read_slot = (s->read_slot + 1) % s->slots;
    }
    s->count = 0;                                /* лічильник розійшовся з FLASH */
    return false;
}

esp_err_t app_store_peek_nth(app_store_t *s, uint32_t n, void *buf, size_t cap, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(s != NULL && s->part != NULL && buf != NULL, ESP_ERR_INVALID_STATE, TAG, "Сховище не ініціалізоване");

    esp_err_t err = ESP_ERR_NOT_FOUND;
    const int64_t t0 = app_stats_now();
    xSemaphoreTake(s->lock, portMAX_DELAY);
    slot_hdr_t h;
    if (n < s->count && seek_valid(s, &h)) {
        /* Обхід кільця від найстарішого запису в порядку запису; n-й дійсний — шуканий */
        uint32_t slot = s->read_slot;
        uint32_t seen = 0;
        for (uint32_t step = 0; step < s->slots; step++, slot = (slot + 1) % s->slots) {
            if (read_hdr(s, slot, &h) != ESP_OK) {
                err = ESP_FAIL;
                break;
            }
            if (classify(&h) != SLOT_VALID) {
                continue;
            }
            if (seen++ < n) {
                continue;
            }
            if (h.len > cap) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            err = esp_partition_read(s->part, slot_offset(s, slot) + APP_STORE_HDR_SIZE, buf, h.len);
            if (err != ESP_OK) {
                break;
            }
            if (esp_rom_crc32_le(0, (const uint8_t *)buf, h.len) == h.crc) {
                if (out_len != NULL) {
                    *out_len = h.len;
                }
                break;
            }
            ESP_LOGW(TAG, "«%s»: CRC запису seq=%u не збігається — запис відкинуто", s->label, (unsigned)h.seq);
            write_state(s, slot, ST_CONSUMED);
            s->count--;
            seen--;
            err = ESP_ERR_NOT_FOUND;
        }
    }
    xSemaphoreGive(s->lock);
    if (err == ESP_OK) {
        ESP_LOGI(STAT_TAG, "flash read «%s» [%u]: %u Б + CRC-32 за %.3f мс", s->label, (unsigned)n,
                 out_len ? (unsigned)*out_len : 0u, (double)(app_stats_now() - t0) / 1000.0);
    }
    return err;
}

esp_err_t app_store_peek(app_store_t *s, void *buf, size_t cap, size_t *out_len)
{
    return app_store_peek_nth(s, 0, buf, cap, out_len);
}

esp_err_t app_store_pop(app_store_t *s)
{
    ESP_RETURN_ON_FALSE(s != NULL && s->part != NULL, ESP_ERR_INVALID_STATE, TAG, "Сховище не ініціалізоване");

    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    slot_hdr_t h;
    if (s->count > 0 && seek_valid(s, &h)) {
        err = write_state(s, s->read_slot, ST_CONSUMED);
        if (err == ESP_OK) {
            s->count--;
            s->read_slot = (s->read_slot + 1) % s->slots;
        }
    }
    xSemaphoreGive(s->lock);
    return err;
}

uint32_t app_store_count(app_store_t *s)
{
    return (s != NULL) ? s->count : 0;
}
