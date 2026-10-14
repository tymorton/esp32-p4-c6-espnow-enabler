/*
 * Host-side unit test for esp_now_hosted_host.c.
 * Runs the real component source against:
 *   - real esp_wifi_remote injected headers (esp_now.h, wifi types for a C6 slave)
 *   - a FreeRTOS-on-pthreads shim
 *   - a fake co-processor that implements the overlay's REQ/RESP/RECV/SEND
 *     behaviour (per esp-hosted-firmware/slave-overlay), with fault injection.
 * Build/run: see run_test.sh
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_now.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_ota.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_now_hosted_rpc.h"
#include "esp_now_hosted_host.h"

/* ================================================== FreeRTOS shim impl == */
struct sem_s { bool mutex; pthread_mutex_t m; pthread_cond_t c; int count; };
static struct timespec deadline(TickType_t ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}
static SemaphoreHandle_t mk(bool mutex, int c)
{
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    s->mutex = mutex; s->count = c;
    pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL);
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return mk(true, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return mk(false, 0); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ms)
{
    pthread_mutex_lock(&s->m);
    struct timespec ts = deadline(ms);
    while (s->count == 0) {
        if (ms == 0) { pthread_mutex_unlock(&s->m); return pdFALSE; }
        int r = ms == portMAX_DELAY ? pthread_cond_wait(&s->c, &s->m) : pthread_cond_timedwait(&s->c, &s->m, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&s->m); return pdFALSE; }
    }
    s->count = 0;
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m); s->count = 1; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
struct queue_s { pthread_mutex_t m; pthread_cond_t c; unsigned len, head, n; size_t item; uint8_t *buf; };
QueueHandle_t xQueueCreate(unsigned len, size_t item)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    q->len = len; q->item = item; q->buf = calloc(len, item);
    pthread_mutex_init(&q->m, NULL); pthread_cond_init(&q->c, NULL);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t ms)
{
    (void)ms;
    pthread_mutex_lock(&q->m);
    if (q->n == q->len) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    memcpy(q->buf + ((q->head + q->n) % q->len) * q->item, it, q->item); q->n++;
    pthread_cond_signal(&q->c); pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *it, TickType_t ms)
{
    pthread_mutex_lock(&q->m);
    struct timespec ts = deadline(ms);
    while (q->n == 0) {
        int r = ms == portMAX_DELAY ? pthread_cond_wait(&q->c, &q->m) : pthread_cond_timedwait(&q->c, &q->m, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    }
    memcpy(it, q->buf + q->head * q->item, q->item); q->head = (q->head + 1) % q->len; q->n--;
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
typedef struct { TaskFunction_t fn; void *arg; } trampoline_t;
static void *tramp(void *p) { trampoline_t t = *(trampoline_t *)p; free(p); t.fn(t.arg); return NULL; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *n, uint32_t st, void *arg, unsigned pr, TaskHandle_t *h, int core)
{
    (void)n; (void)st; (void)pr; (void)h; (void)core;
    pthread_t th; trampoline_t *t = malloc(sizeof *t); t->fn = fn; t->arg = arg;
    pthread_create(&th, NULL, tramp, t); pthread_detach(th);
    return pdPASS;
}
int64_t esp_timer_get_time(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000; }
const char *esp_err_to_name(esp_err_t e) { static char b[16]; snprintf(b, sizeof b, "0x%x", e); return b; }
esp_err_t esp_wifi_get_mac(wifi_interface_t i, uint8_t mac[6]) { (void)i; static const uint8_t m[6] = {0x60,0x55,0xF9,1,2,3}; memcpy(mac, m, 6); return ESP_OK; }

/* ============================================== fake esp_hosted + CP ===== */
typedef void (*custom_cb_t)(uint32_t, const uint8_t *, size_t, void *);
static struct { uint32_t id; custom_cb_t cb; void *ctx; } g_handlers[8];
static int g_handler_n;
esp_err_t esp_hosted_register_custom_callback(uint32_t id, custom_cb_t cb, void *ctx)
{
    if (g_handler_n >= 3) return ESP_ERR_NO_MEM;              /* esp_hosted 2.x default: 3 handlers */
    g_handlers[g_handler_n].id = id; g_handlers[g_handler_n].cb = cb; g_handlers[g_handler_n].ctx = ctx; g_handler_n++;
    return ESP_OK;
}

typedef struct { uint32_t id; uint16_t len; int delay_ms; uint8_t data[1600]; } msg_t;
static QueueHandle_t q_to_cp, q_to_host;

/* fault injection / observation */
static atomic_bool cp_overlay = true;     /* false = stock firmware: never answers REQ */
static atomic_int  cp_delay_next_resp_ms; /* delay the next RESP (to force a host timeout) */
static atomic_bool cp_echo = true;        /* echo each SEND back as a RECV from the dest MAC */
static atomic_int  cp_add_peer_calls, cp_send_calls;
static bool cp_inited;
static uint8_t cp_peers[20][6]; static int cp_npeers;

static void to_host(uint32_t id, const void *d, size_t n, int delay)
{
    msg_t m = { .id = id, .len = (uint16_t)n, .delay_ms = delay };
    memcpy(m.data, d, n);
    msg_t *p = malloc(sizeof m); *p = m;
    assert(xQueueSend(q_to_host, &p, 0) == pdTRUE);
}

esp_err_t esp_hosted_send_custom_data(uint32_t id, const uint8_t *d, size_t n)
{
    assert(id == ESP_NOW_HOSTED_MSG_REQ && n <= 1600);
    msg_t *p = malloc(sizeof *p); p->id = id; p->len = (uint16_t)n; memcpy(p->data, d, n);
    return xQueueSend(q_to_cp, &p, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void *cp_thread(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t *m; xQueueReceive(q_to_cp, &m, portMAX_DELAY);
        if (!cp_overlay) { free(m); continue; }
        const esp_now_hosted_req_t *rq = (const esp_now_hosted_req_t *)m->data;
        assert(m->len >= sizeof *rq && m->len == sizeof *rq + rq->payload_len);   /* framing check */
        uint8_t rb[sizeof(esp_now_hosted_resp_t) + 8] = {0};
        esp_now_hosted_resp_t *r = (esp_now_hosted_resp_t *)rb;
        r->opcode = rq->opcode; r->seq = rq->seq; r->status = ESP_OK;
        size_t rl = sizeof *r;
        const uint8_t *pl = rq->payload;
        switch (rq->opcode) {
        case ESP_NOW_HOSTED_OP_INIT: cp_inited = true; break;
        case ESP_NOW_HOSTED_OP_DEINIT: cp_inited = false; cp_npeers = 0; break;
        case ESP_NOW_HOSTED_OP_GET_VERSION: { uint32_t v = 2; memcpy(r->ret, &v, 4); r->ret_len = 4; rl += 4;
            if (!cp_inited) { r->status = ESP_ERR_ESPNOW_NOT_INIT; }
            break; }
        case ESP_NOW_HOSTED_OP_ADD_PEER: { cp_add_peer_calls++;
            const esp_now_hosted_peer_t *p = (const void *)pl;
            for (int i = 0; i < cp_npeers; i++) if (!memcmp(cp_peers[i], p->peer_addr, 6)) { r->status = ESP_ERR_ESPNOW_EXIST; break; }
            if (r->status == ESP_OK) memcpy(cp_peers[cp_npeers++], p->peer_addr, 6);
            break; }
        case ESP_NOW_HOSTED_OP_MOD_PEER: break;
        case ESP_NOW_HOSTED_OP_DEL_PEER: { int k = -1;
            for (int i = 0; i < cp_npeers; i++) if (!memcmp(cp_peers[i], pl, 6)) k = i;
            if (k < 0) r->status = ESP_ERR_ESPNOW_NOT_FOUND; else memcpy(cp_peers[k], cp_peers[--cp_npeers], 6);
            break; }
        case ESP_NOW_HOSTED_OP_IS_PEER_EXIST: { r->ret[0] = 0;
            for (int i = 0; i < cp_npeers; i++) if (!memcmp(cp_peers[i], pl, 6)) r->ret[0] = 1;
            r->ret_len = 1; rl += 1; break; }
        case ESP_NOW_HOSTED_OP_SET_PMK: break;
        case ESP_NOW_HOSTED_OP_SEND: {
            cp_send_calls++;
            const esp_now_hosted_send_req_t *s = (const void *)pl;
            assert(s->data_len == rq->payload_len - sizeof *s);
            if (!cp_inited) { r->status = ESP_ERR_ESPNOW_NOT_INIT; break; }
            uint8_t dst[6]; memcpy(dst, s->has_addr ? s->peer_addr : (const uint8_t[6]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, 6);
            int d = atomic_exchange(&cp_delay_next_resp_ms, 0);
            to_host(ESP_NOW_HOSTED_MSG_RESP, rb, rl, d);           /* RESP first, like the overlay */
            esp_now_hosted_send_evt_t se; memcpy(se.des_addr, dst, 6); se.status = 0;
            to_host(ESP_NOW_HOSTED_MSG_SEND, &se, sizeof se, 0);
            if (cp_echo) {
                uint8_t eb[sizeof(esp_now_hosted_recv_evt_t) + 1470];
                esp_now_hosted_recv_evt_t *e = (void *)eb;
                memcpy(e->src_addr, dst, 6); memset(e->des_addr, 0xFF, 6);
                e->rssi = -42; e->channel = 6; e->data_len = s->data_len; memcpy(e->data, s->data, s->data_len);
                to_host(ESP_NOW_HOSTED_MSG_RECV, eb, sizeof *e + s->data_len, 0);
            }
            free(m); continue; }
        default: r->status = ESP_ERR_NOT_SUPPORTED;
        }
        to_host(ESP_NOW_HOSTED_MSG_RESP, rb, rl, atomic_exchange(&cp_delay_next_resp_ms, 0));
        free(m);
    }
    return NULL;
}

/* host RX thread: plays esp_hosted's RPC RX thread delivering custom data */
static void *host_rx_thread(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t *m; xQueueReceive(q_to_host, &m, portMAX_DELAY);
        if (m->delay_ms) usleep(m->delay_ms * 1000);
        for (int i = 0; i < g_handler_n; i++)
            if (g_handlers[i].id == m->id) g_handlers[i].cb(m->id, m->data, m->len, g_handlers[i].ctx);
        free(m);
    }
    return NULL;
}

/* fake slave OTA */
static size_t ota_bytes; static int ota_state;
esp_err_t esp_hosted_slave_ota_begin(void) { ota_bytes = 0; ota_state = 1; return ESP_OK; }
esp_err_t esp_hosted_slave_ota_write(uint8_t *d, uint32_t n) { (void)d; assert(ota_state == 1 && n <= 1400); ota_bytes += n; return ESP_OK; }
esp_err_t esp_hosted_slave_ota_end(void) { ota_state = 2; return ESP_OK; }
esp_err_t esp_hosted_slave_ota_activate(void) { assert(ota_state == 2); ota_state = 3; return ESP_OK; }

/* ============================================================== tests ==== */
static sem_t got_recv, got_send;
static atomic_int recv_count, send_count, reentrant_sends;
static uint8_t last_rx[1470]; static int last_rx_len; static int8_t last_rssi; static uint8_t last_src[6];
static atomic_bool reply_from_cb;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    memcpy(last_rx, data, len); last_rx_len = len; last_rssi = info->rx_ctrl->rssi; memcpy(last_src, info->src_addr, 6);
    recv_count++;
    if (reply_from_cb) {                     /* re-entrancy: native-style "reply from callback" */
        reply_from_cb = false;
        cp_echo = false;
        if (esp_now_send(info->src_addr, (const uint8_t *)"pong", 4) == ESP_OK) reentrant_sends++;
        cp_echo = true;
    }
    sem_post(&got_recv);
}
static void on_send(const esp_now_send_info_t *tx, esp_now_send_status_t st)
{
    assert(tx && tx->des_addr && tx->src_addr && st == ESP_NOW_SEND_SUCCESS);
    send_count++; sem_post(&got_send);
}
static int wait_sem(sem_t *s, int ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return sem_timedwait(s, &ts);
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } else printf("  ok  %s\n", #c); } while (0)

int main(void)
{
    q_to_cp = xQueueCreate(64, sizeof(msg_t *));
    q_to_host = xQueueCreate(64, sizeof(msg_t *));
    pthread_t a, b; pthread_create(&a, NULL, cp_thread, NULL); pthread_create(&b, NULL, host_rx_thread, NULL);
    sem_init(&got_recv, 0, 0); sem_init(&got_send, 0, 0);
    const uint8_t peer[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

    puts("1. stock co-processor firmware (no overlay) is detected, calls time out cleanly");
    cp_overlay = false;
    CHECK(esp_now_hosted_host_cp_supported() == false);
    CHECK(esp_now_init() == ESP_ERR_TIMEOUT);
    CHECK(g_handler_n == 3);
    cp_overlay = true;
    CHECK(esp_now_hosted_host_cp_supported() == true);

    puts("2. API guards before init");
    CHECK(esp_now_send(peer, (const uint8_t *)"x", 1) == ESP_ERR_ESPNOW_NOT_INIT);
    CHECK(esp_now_register_recv_cb(on_recv) == ESP_ERR_ESPNOW_NOT_INIT);

    puts("3. init, callbacks, encrypted peer, mirror");
    CHECK(esp_now_init() == ESP_OK);
    CHECK(esp_now_register_recv_cb(on_recv) == ESP_OK);
    CHECK(esp_now_register_send_cb(on_send) == ESP_OK);
    uint8_t pmk[16] = "pmk1234567890123"; CHECK(esp_now_set_pmk(pmk) == ESP_OK);
    esp_now_peer_info_t pi = {0}; memcpy(pi.peer_addr, peer, 6); pi.channel = 6; pi.ifidx = WIFI_IF_STA; pi.encrypt = true;
    memcpy(pi.lmk, "lmk1234567890123", 16);
    CHECK(esp_now_add_peer(&pi) == ESP_OK);
    CHECK(esp_now_add_peer(&pi) == ESP_ERR_ESPNOW_EXIST);
    CHECK(esp_now_is_peer_exist(peer) == true);
    esp_now_peer_num_t num; CHECK(esp_now_get_peer_num(&num) == ESP_OK && num.total_num == 1 && num.encrypt_num == 1);
    esp_now_peer_info_t got; CHECK(esp_now_get_peer(peer, &got) == ESP_OK && got.encrypt && got.channel == 6);
    uint32_t ver = 0; CHECK(esp_now_get_version(&ver) == ESP_OK && ver == 2);

    puts("4. unicast send -> send callback + echoed receive callback with metadata");
    uint8_t big[1470]; for (int i = 0; i < 1470; i++) big[i] = (uint8_t)i;
    CHECK(esp_now_send(peer, big, sizeof big) == ESP_OK);
    CHECK(wait_sem(&got_send, 1000) == 0);
    CHECK(wait_sem(&got_recv, 1000) == 0);
    CHECK(last_rx_len == 1470 && memcmp(last_rx, big, 1470) == 0);
    CHECK(last_rssi == -42 && memcmp(last_src, peer, 6) == 0);
    CHECK(esp_now_send(peer, big, 1471) == ESP_ERR_ESPNOW_ARG);
    CHECK(esp_now_send(NULL, (const uint8_t *)"bcast", 5) == ESP_OK);
    CHECK(wait_sem(&got_send, 1000) == 0 && wait_sem(&got_recv, 1000) == 0);

    puts("5. esp_now_send() from inside the receive callback does not deadlock");
    reply_from_cb = true;
    CHECK(esp_now_send(peer, (const uint8_t *)"ping", 4) == ESP_OK);
    CHECK(wait_sem(&got_recv, 1000) == 0);
    CHECK(wait_sem(&got_send, 1000) == 0);       /* ping's tx status */
    CHECK(wait_sem(&got_send, 1000) == 0);       /* pong's tx status (sent from callback) */
    CHECK(reentrant_sends == 1);

    puts("6. a late response is not mistaken for the next request's answer");
    esp_now_hosted_host_stats_t st, st0;
    esp_now_hosted_host_get_stats(&st0);
    cp_delay_next_resp_ms = 400;                 /* > 150 ms test timeout */
    CHECK(esp_now_is_peer_exist(peer) == true);  /* falls back to mirror on timeout */
    esp_now_hosted_host_get_stats(&st); CHECK(st.rpc_timeouts == st0.rpc_timeouts + 1);
    usleep(450 * 1000);                          /* stale RESP arrives now */
    const uint8_t nobody[6] = {1, 2, 3, 4, 5, 6};
    CHECK(esp_now_is_peer_exist(nobody) == false);
    esp_now_hosted_host_get_stats(&st); CHECK(st.rpc_stale_resp >= 1);

    puts("7. co-processor reboot -> reinit restores PMK + peers");
    int adds_before = cp_add_peer_calls;
    cp_inited = false; cp_npeers = 0;            /* simulate C6 reset */
    CHECK(esp_now_hosted_host_reinit() == ESP_OK);
    CHECK(cp_add_peer_calls == adds_before + 1 && cp_npeers == 1);

    puts("8. delete peer, deinit");
    CHECK(esp_now_del_peer(peer) == ESP_OK);
    CHECK(esp_now_get_peer_num(&num) == ESP_OK && num.total_num == 0);
    CHECK(esp_now_deinit() == ESP_OK);
    CHECK(esp_now_send(peer, big, 4) == ESP_ERR_ESPNOW_NOT_INIT);

    puts("9. co-processor firmware update helper chunks and activates");
    static uint8_t img[100000]; img[0] = 0xE9;
    CHECK(esp_now_hosted_host_cp_update(img, sizeof img) == ESP_OK && ota_bytes == sizeof img && ota_state == 3);
    CHECK(esp_now_hosted_host_cp_update((const uint8_t *)"nope", 4) == ESP_ERR_INVALID_ARG);

    puts("10. unsupported APIs link and fail loudly");
    CHECK(esp_now_set_wake_window(10) == ESP_ERR_NOT_SUPPORTED);

    esp_now_hosted_host_get_stats(&st);
    printf("stats: calls=%u timeouts=%u stale=%u rx=%u dropped=%u tx_events=%u\n",
           st.rpc_calls, st.rpc_timeouts, st.rpc_stale_resp, st.rx_frames, st.rx_dropped, st.tx_status_events);
    puts("ALL TESTS PASSED");
    return 0;
}
