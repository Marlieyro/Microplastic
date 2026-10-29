//
// Created by user on 27.10.26.
//
// Мінімальний DNS-сервер для Captive Portal (RFC 1035).
//
// Формат відповіді завжди будується "з нуля":
//   [заголовок 12 Б][перше питання з запиту][0 або 1 запис A]
// Розділи Authority/Additional (наприклад, EDNS OPT від Windows/macOS)
// у відповідь не копіюються — так відповідь завжди коректна.
//

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "appDNSServer.h"

#define DNS_PORT                53
#define DNS_BUF_SIZE            512     /* максимальний розмір DNS-повідомлення по UDP */
#define DNS_HEADER_LEN          12
#define DNS_ANSWER_LEN          16      /* ptr(2) type(2) class(2) ttl(4) rdlen(2) ipv4(4) */
#define DNS_NAME_MAX_LEN        255
/* Малий TTL: після переходу ноутбука/телефону в "справжню" мережу кеш
 * зі старою адресою 192.168.4.1 швидко зникає */
#define DNS_ANSWER_TTL_SEC      10
/* Період перевірки прапорця зупинки задачі */
#define DNS_RX_TIMEOUT_MS       500
#define DNS_TASK_STACK_SIZE     3072
#define DNS_TASK_PRIORITY       5

#define DNS_FLAG_QR             0x8000  /* 1 — відповідь */
#define DNS_FLAG_AA             0x0400  /* авторитетна відповідь */
#define DNS_FLAG_RD             0x0100  /* рекурсія бажана (копіюється з запиту) */
#define DNS_OPCODE_MASK         0x7800
#define DNS_RCODE_FORMERR       1
#define DNS_RCODE_NOTIMP        4

#define DNS_TYPE_A              1
#define DNS_TYPE_ANY            255
#define DNS_CLASS_IN            1

static const char *TAG = "DNS_SERVER";

static TaskHandle_t      s_task = NULL;
static SemaphoreHandle_t s_stopped_sem = NULL;
static volatile bool     s_running = false;
static esp_ip4_addr_t    s_answer_ip;

/* Буфери статичні, щоб не роздувати стек задачі */
static uint8_t s_rx_buf[DNS_BUF_SIZE];
static uint8_t s_tx_buf[DNS_BUF_SIZE];

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/*
 * Розбирає перше питання запиту.
 * q_end  — зміщення першого байта ПІСЛЯ питання (ім'я + QTYPE + QCLASS).
 */
static bool dns_parse_question(const uint8_t *req, size_t req_len,
                               size_t *q_end, uint16_t *qtype, uint16_t *qclass)
{
    size_t pos = DNS_HEADER_LEN;
    size_t name_len = 0;

    for (;;) {
        if (pos >= req_len) {
            return false;
        }
        const uint8_t label_len = req[pos];
        if (label_len == 0) {
            pos++;
            break;
        }
        if ((label_len & 0xC0) != 0) {
            return false;       /* стиснення імен у питанні запиту не використовується */
        }
        name_len += (size_t)label_len + 1;
        if (name_len > DNS_NAME_MAX_LEN) {
            return false;
        }
        pos += (size_t)label_len + 1;
    }

    if (pos + 4 > req_len) {
        return false;
    }
    *qtype  = rd16(&req[pos]);
    *qclass = rd16(&req[pos + 2]);
    *q_end  = pos + 4;
    return true;
}

/*
 * Формує відповідь. Повертає її довжину або 0, якщо відповідати не треба.
 */
static size_t dns_build_reply(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap)
{
    if (req_len < DNS_HEADER_LEN || rsp_cap < DNS_HEADER_LEN) {
        return 0;
    }

    const uint16_t flags = rd16(&req[2]);
    if ((flags & DNS_FLAG_QR) != 0) {
        return 0;               /* це відповідь, а не запит — ігноруємо */
    }

    const uint16_t opcode = flags & DNS_OPCODE_MASK;
    size_t   q_end  = 0;
    uint16_t qtype  = 0;
    uint16_t qclass = 0;
    const bool has_question = (opcode == 0) && (rd16(&req[4]) >= 1) &&
                              dns_parse_question(req, req_len, &q_end, &qtype, &qclass);

    uint16_t rsp_flags = DNS_FLAG_QR | DNS_FLAG_AA | opcode | (flags & DNS_FLAG_RD);
    if (opcode != 0) {
        rsp_flags |= DNS_RCODE_NOTIMP;
    } else if (!has_question) {
        rsp_flags |= DNS_RCODE_FORMERR;
    }

    /* Заголовок */
    rsp[0] = req[0];            /* ID копіюється без змін */
    rsp[1] = req[1];
    wr16(&rsp[2], rsp_flags);
    wr16(&rsp[4], has_question ? 1 : 0);    /* QDCOUNT */
    wr16(&rsp[6], 0);                        /* ANCOUNT (оновлюється нижче) */
    wr16(&rsp[8], 0);                        /* NSCOUNT */
    wr16(&rsp[10], 0);                       /* ARCOUNT */
    size_t len = DNS_HEADER_LEN;

    if (!has_question) {
        return len;
    }

    /* Питання копіюється як є */
    const size_t q_len = q_end - DNS_HEADER_LEN;
    if (len + q_len + DNS_ANSWER_LEN > rsp_cap) {
        return 0;
    }
    memcpy(&rsp[len], &req[DNS_HEADER_LEN], q_len);
    len += q_len;

    /* Відповідь A — лише для IPv4-запитів класу IN */
    if (qclass == DNS_CLASS_IN && (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY)) {
        uint8_t *ans = &rsp[len];
        wr16(&ans[0], 0xC000 | DNS_HEADER_LEN);  /* вказівник на ім'я в питанні */
        wr16(&ans[2], DNS_TYPE_A);
        wr16(&ans[4], DNS_CLASS_IN);
        ans[6] = (uint8_t)((DNS_ANSWER_TTL_SEC >> 24) & 0xFF);
        ans[7] = (uint8_t)((DNS_ANSWER_TTL_SEC >> 16) & 0xFF);
        ans[8] = (uint8_t)((DNS_ANSWER_TTL_SEC >> 8) & 0xFF);
        ans[9] = (uint8_t)(DNS_ANSWER_TTL_SEC & 0xFF);
        wr16(&ans[10], 4);                       /* RDLENGTH */
        memcpy(&ans[12], &s_answer_ip.addr, 4);  /* адреса вже в мережевому порядку байтів */
        len += DNS_ANSWER_LEN;
        wr16(&rsp[6], 1);
    }
    /* Для AAAA та інших типів: NOERROR без записів (NODATA) */
    return len;
}

static void dns_serve(void)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Не вдалося створити сокет: errno %d", errno);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Не вдалося зайняти порт %d: errno %d", DNS_PORT, errno);
        close(sock);
        return;
    }

    /* Таймаут прийому — щоб задача періодично перевіряла s_running */
    const struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = DNS_RX_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS-сервер запущено (порт %d)", DNS_PORT);

    while (s_running) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        const int rx_len = recvfrom(sock, s_rx_buf, sizeof(s_rx_buf), 0,
                                    (struct sockaddr *)&src_addr, &src_len);
        if (rx_len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

        const size_t tx_len = dns_build_reply(s_rx_buf, (size_t)rx_len, s_tx_buf, sizeof(s_tx_buf));
        if (tx_len > 0) {
            if (sendto(sock, s_tx_buf, tx_len, 0, (struct sockaddr *)&src_addr, src_len) < 0) {
                ESP_LOGW(TAG, "sendto: errno %d", errno);
            }
        }
    }

    close(sock);
    ESP_LOGI(TAG, "DNS-сервер зупинено");
}

static void dns_server_task(void *arg)
{
    (void)arg;
    dns_serve();
    xSemaphoreGive(s_stopped_sem);
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(esp_ip4_addr_t answer_ip)
{
    if (s_task != NULL) {
        return ESP_OK;          /* вже працює */
    }
    if (s_stopped_sem == NULL) {
        s_stopped_sem = xSemaphoreCreateBinary();
        if (s_stopped_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_stopped_sem, 0);   /* скидання можливого старого стану */

    s_answer_ip = answer_ip;
    s_running = true;
    if (xTaskCreate(dns_server_task, "dns_server", DNS_TASK_STACK_SIZE, NULL,
                    DNS_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_server_stop(void)
{
    if (s_task == NULL) {
        return;
    }
    s_running = false;
    /* Задача помітить прапорець максимум через DNS_RX_TIMEOUT_MS */
    if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(DNS_RX_TIMEOUT_MS * 4)) != pdTRUE) {
        ESP_LOGW(TAG, "DNS-задача не завершилась вчасно");
    }
    s_task = NULL;
}
