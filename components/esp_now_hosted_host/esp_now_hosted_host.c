/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_now_hosted_host — host-side implementation of the native esp_now_* API over
 * esp_hosted CustomRpc. Independent C implementation of the wire contract in
 * esp_now_hosted_rpc.h (Apache-2.0, github.com/esphome/esp-hosted-firmware).
 *
 *  app ── esp_now_*() ──► rpc(): REQ{op,seq,payload} ─► co-processor ─► native esp_now_*()
 *                          ◄── RESP{op,seq,status,ret} (matched by seq, timeout)
 *  co-processor RECV/SEND events ─► s_evt_q ─► dispatch task ─► app callbacks
 *
 * Threading: one request in flight at a time (s_lock). esp_hosted delivers RESP
 * and events on its RPC RX thread; we only copy there and never block it, and
 * app callbacks run on our dispatch task, so callbacks may call esp_now_*().
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_hosted_misc.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_now_hosted_rpc.h"
#include "esp_now_hosted_host.h"

#if !defined(CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER) && !defined(CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA)
#error "esp_now_hosted_host needs esp_hosted peer-data transfer: 2.x CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y, 3.x CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA=y"
#endif

#ifndef CONFIG_ESP_NOW_HOSTED_HOST_TIMEOUT_MS
#define CONFIG_ESP_NOW_HOSTED_HOST_TIMEOUT_MS ESP_NOW_HOSTED_TIMEOUT_MS
#endif
#ifndef CONFIG_ESP_NOW_HOSTED_HOST_QUEUE_LEN
#define CONFIG_ESP_NOW_HOSTED_HOST_QUEUE_LEN 16
#endif
#ifndef CONFIG_ESP_NOW_HOSTED_HOST_TASK_STACK
#define CONFIG_ESP_NOW_HOSTED_HOST_TASK_STACK 4096
#endif
#ifndef CONFIG_ESP_NOW_HOSTED_HOST_TASK_PRIO
#define CONFIG_ESP_NOW_HOSTED_HOST_TASK_PRIO 10
#endif
#ifndef CONFIG_ESP_NOW_HOSTED_HOST_TASK_CORE
#define CONFIG_ESP_NOW_HOSTED_HOST_TASK_CORE -1
#endif
#define TASK_CORE ((CONFIG_ESP_NOW_HOSTED_HOST_TASK_CORE) < 0 ? tskNO_AFFINITY : (CONFIG_ESP_NOW_HOSTED_HOST_TASK_CORE))

#define NEW_SEND_CB (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0))
#define MAX_PEERS   ESP_NOW_MAX_TOTAL_PEER_NUM

static const char *TAG = "esp_now_hosted";

/* ------------------------------------------------------------------ state -- */
static SemaphoreHandle_t s_lock;          /* serializes requests             */
static SemaphoreHandle_t s_resp_sem;      /* RESP arrived for waiting seq     */
static QueueHandle_t     s_evt_q;         /* RECV/SEND events -> dispatch     */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool              s_runtime_ready;

static uint8_t  s_seq;
static int      s_wait_seq = -1;          /* -1: nobody waiting (guarded by s_mux) */
static uint8_t  s_wait_op;
static uint8_t  s_resp_buf[sizeof(esp_now_hosted_resp_t) + 8];
static size_t   s_resp_len;

static uint8_t  s_req_buf[sizeof(esp_now_hosted_req_t) + ESP_NOW_HOSTED_MAX_PAYLOAD];

static volatile esp_now_recv_cb_t s_recv_cb;
static volatile esp_now_send_cb_t s_send_cb;
static bool     s_inited;
static uint8_t  s_own_mac[6];

static bool     s_pmk_set;
static uint8_t  s_pmk[ESP_NOW_KEY_LEN];

static esp_now_peer_info_t s_peers[MAX_PEERS];
static bool     s_peer_used[MAX_PEERS];
static int      s_fetch_idx;

static esp_now_hosted_host_stats_t s_stats;

typedef struct {
    uint32_t msg_id;
    uint16_t len;
    uint8_t  data[];
} evt_t;

/* -------------------------------------------------------- transport side -- */
static void on_resp(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (len < sizeof(esp_now_hosted_resp_t) || len > sizeof(s_resp_buf)) return;
    const esp_now_hosted_resp_t *r = (const esp_now_hosted_resp_t *)data;
    bool matched = false;
    portENTER_CRITICAL(&s_mux);
    if (s_wait_seq >= 0 && r->seq == (uint8_t)s_wait_seq && r->opcode == s_wait_op) {
        memcpy(s_resp_buf, data, len);
        s_resp_len = len;
        s_wait_seq = -1;
        matched = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (matched) xSemaphoreGive(s_resp_sem);
    else s_stats.rpc_stale_resp++;
}

static void on_event(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (len > sizeof(esp_now_hosted_recv_evt_t) + ESP_NOW_HOSTED_MAX_FRAME) { s_stats.rx_dropped++; return; }
    evt_t *e = malloc(sizeof(evt_t) + len);
    if (!e) { s_stats.rx_dropped++; return; }
    e->msg_id = msg_id;
    e->len = (uint16_t)len;
    memcpy(e->data, data, len);
    if (xQueueSend(s_evt_q, &e, 0) != pdTRUE) { free(e); s_stats.rx_dropped++; }
}

static void dispatch_recv(const evt_t *e)
{
    if (e->len < sizeof(esp_now_hosted_recv_evt_t)) return;
    const esp_now_hosted_recv_evt_t *r = (const esp_now_hosted_recv_evt_t *)e->data;
    if ((size_t)r->data_len + sizeof(*r) > e->len) return;          /* malformed */
    esp_now_recv_cb_t cb = s_recv_cb;
    if (!cb) return;
    uint8_t src[6], dst[6];
    memcpy(src, r->src_addr, 6);
    memcpy(dst, r->des_addr, 6);
    wifi_pkt_rx_ctrl_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.rssi = r->rssi;
    rx.channel = r->channel;
    rx.timestamp = (uint32_t)esp_timer_get_time();   /* host receive time, us */
    esp_now_recv_info_t info = { .src_addr = src, .des_addr = dst, .rx_ctrl = &rx };
    s_stats.rx_frames++;
    cb(&info, r->data, r->data_len);
}

static void dispatch_send(const evt_t *e)
{
    if (e->len < sizeof(esp_now_hosted_send_evt_t)) return;
    const esp_now_hosted_send_evt_t *s = (const esp_now_hosted_send_evt_t *)e->data;
    esp_now_send_cb_t cb = s_send_cb;
    if (!cb) return;
    uint8_t dst[6];
    memcpy(dst, s->des_addr, 6);
    s_stats.tx_status_events++;
#if NEW_SEND_CB
    esp_now_send_info_t info;
    memset(&info, 0, sizeof(info));
    info.des_addr = dst;
    info.src_addr = s_own_mac;
    info.ifidx = WIFI_IF_STA;
    info.tx_status = s->status == ESP_NOW_SEND_SUCCESS ? WIFI_SEND_SUCCESS : WIFI_SEND_FAIL;
    cb(&info, (esp_now_send_status_t)s->status);
#else
    cb(dst, (esp_now_send_status_t)s->status);
#endif
}

static void dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        evt_t *e = NULL;
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) != pdTRUE || !e) continue;
        if (e->msg_id == ESP_NOW_HOSTED_MSG_RECV) dispatch_recv(e);
        else if (e->msg_id == ESP_NOW_HOSTED_MSG_SEND) dispatch_send(e);
        free(e);
    }
}

static esp_err_t runtime_init(void)
{
    if (s_runtime_ready) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    s_evt_q = xQueueCreate(CONFIG_ESP_NOW_HOSTED_HOST_QUEUE_LEN, sizeof(evt_t *));
    if (!s_lock || !s_resp_sem || !s_evt_q) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(dispatch_task, "espnow_hosted", CONFIG_ESP_NOW_HOSTED_HOST_TASK_STACK, NULL,
                                CONFIG_ESP_NOW_HOSTED_HOST_TASK_PRIO, NULL, TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RESP, on_resp, NULL);
    if (err == ESP_OK) err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_RECV, on_event, NULL);
    if (err == ESP_OK) err = esp_hosted_register_custom_callback(ESP_NOW_HOSTED_MSG_SEND, on_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "custom RPC handler registration failed (%s); raise the esp_hosted "
                      "MAX_CUSTOM_MSG_HANDLERS setting (needs 3 for ESP-NOW)", esp_err_to_name(err));
        return err;
    }
    s_runtime_ready = true;
    return ESP_OK;
}

static bool s_last_had_resp;                      /* last exchange got a matching RESP */

static inline uint8_t *req_payload(void) { return ((esp_now_hosted_req_t *)s_req_buf)->payload; }

/* Exchange with s_lock held and the payload already in req_payload(). */
static esp_err_t rpc_locked(uint8_t op, uint16_t len, void *ret, uint16_t ret_cap)
{
    if (len > ESP_NOW_HOSTED_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_resp_sem, 0);                     /* drop any stale give */

    esp_now_hosted_req_t *req = (esp_now_hosted_req_t *)s_req_buf;
    req->opcode = op;
    req->seq = ++s_seq;
    req->payload_len = len;

    portENTER_CRITICAL(&s_mux);
    s_wait_seq = req->seq;
    s_wait_op = op;
    portEXIT_CRITICAL(&s_mux);

    s_stats.rpc_calls++;
    s_last_had_resp = false;
    esp_err_t err = esp_hosted_send_custom_data(ESP_NOW_HOSTED_MSG_REQ, s_req_buf, sizeof(*req) + len);
    if (err == ESP_OK &&
        xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(CONFIG_ESP_NOW_HOSTED_HOST_TIMEOUT_MS)) == pdTRUE) {
        const esp_now_hosted_resp_t *r = (const esp_now_hosted_resp_t *)s_resp_buf;
        s_last_had_resp = true;
        err = (esp_err_t)r->status;
        if (ret && ret_cap) {
            uint16_t n = r->ret_len;
            if (n > ret_cap) n = ret_cap;
            if (sizeof(*r) + n > s_resp_len) n = 0;
            memcpy(ret, r->ret, n);
        }
    } else {
        portENTER_CRITICAL(&s_mux);
        s_wait_seq = -1;
        portEXIT_CRITICAL(&s_mux);
        if (err == ESP_OK) {
            err = ESP_ERR_TIMEOUT;
            s_stats.rpc_timeouts++;
            ESP_LOGW(TAG, "op %u timed out: co-processor firmware without the ESP-NOW overlay?", op);
        }
    }
    return err;
}

/* One request/response exchange. payload/ret may be NULL. */
static esp_err_t rpc(uint8_t op, const void *payload, uint16_t len, void *ret, uint16_t ret_cap)
{
    esp_err_t err = runtime_init();
    if (err != ESP_OK) return err;
    if (len > ESP_NOW_HOSTED_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (len) memcpy(req_payload(), payload, len);
    err = rpc_locked(op, len, ret, ret_cap);
    xSemaphoreGive(s_lock);
    return err;
}

/* ------------------------------------------------------------ peer mirror -- */
static int mirror_find(const uint8_t *mac)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peer_used[i] && memcmp(s_peers[i].peer_addr, mac, 6) == 0) return i;
    return -1;
}

static void mirror_put(const esp_now_peer_info_t *p)
{
    int i = mirror_find(p->peer_addr);
    for (int k = 0; i < 0 && k < MAX_PEERS; k++) if (!s_peer_used[k]) i = k;
    if (i < 0) return;
    s_peers[i] = *p;
    s_peers[i].priv = NULL;
    s_peer_used[i] = true;
}

static void peer_to_wire(const esp_now_peer_info_t *p, esp_now_hosted_peer_t *w)
{
    memset(w, 0, sizeof(*w));
    memcpy(w->peer_addr, p->peer_addr, 6);
    memcpy(w->lmk, p->lmk, 16);
    w->channel = p->channel;
    w->ifidx = (uint8_t)p->ifidx;
    w->encrypt = p->encrypt ? 1 : 0;
}

/* ------------------------------------------------------------- public API -- */
esp_err_t esp_now_init(void)
{
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_INIT, NULL, 0, NULL, 0);
    if (err == ESP_OK) {
        s_inited = true;
        if (esp_wifi_get_mac(WIFI_IF_STA, s_own_mac) != ESP_OK) memset(s_own_mac, 0, 6);
    }
    return err;
}

esp_err_t esp_now_deinit(void)
{
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_DEINIT, NULL, 0, NULL, 0);
    s_inited = false;
    memset(s_peer_used, 0, sizeof(s_peer_used));
    s_recv_cb = NULL;
    s_send_cb = NULL;
    return err;
}

esp_err_t esp_now_get_version(uint32_t *version)
{
    if (!version) return ESP_ERR_ESPNOW_ARG;
    return rpc(ESP_NOW_HOSTED_OP_GET_VERSION, NULL, 0, version, sizeof(*version));
}

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    s_recv_cb = cb;
    return ESP_OK;
}
esp_err_t esp_now_unregister_recv_cb(void) { s_recv_cb = NULL; return s_inited ? ESP_OK : ESP_ERR_ESPNOW_NOT_INIT; }

esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    s_send_cb = cb;
    return ESP_OK;
}
esp_err_t esp_now_unregister_send_cb(void) { s_send_cb = NULL; return s_inited ? ESP_OK : ESP_ERR_ESPNOW_NOT_INIT; }

esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!data || len == 0 || len > ESP_NOW_HOSTED_MAX_FRAME) return ESP_ERR_ESPNOW_ARG;
    esp_err_t err = runtime_init();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);             /* build in place: no 1.5 KB stack copy */
    esp_now_hosted_send_req_t *s = (esp_now_hosted_send_req_t *)req_payload();
    s->has_addr = peer_addr ? 1 : 0;
    if (peer_addr) memcpy(s->peer_addr, peer_addr, 6); else memset(s->peer_addr, 0, 6);
    s->data_len = (uint16_t)len;
    memcpy(s->data, data, len);
    err = rpc_locked(ESP_NOW_HOSTED_OP_SEND, (uint16_t)(sizeof(*s) + len), NULL, 0);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!peer) return ESP_ERR_ESPNOW_ARG;
    esp_now_hosted_peer_t w;
    peer_to_wire(peer, &w);
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_ADD_PEER, &w, sizeof(w), NULL, 0);
    if (err == ESP_OK) mirror_put(peer);
    return err;
}

esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!peer) return ESP_ERR_ESPNOW_ARG;
    esp_now_hosted_peer_t w;
    peer_to_wire(peer, &w);
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_MOD_PEER, &w, sizeof(w), NULL, 0);
    if (err == ESP_OK) mirror_put(peer);
    return err;
}

esp_err_t esp_now_del_peer(const uint8_t *peer_addr)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!peer_addr) return ESP_ERR_ESPNOW_ARG;
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_DEL_PEER, peer_addr, 6, NULL, 0);
    int i = mirror_find(peer_addr);
    if (err == ESP_OK && i >= 0) s_peer_used[i] = false;
    return err;
}

bool esp_now_is_peer_exist(const uint8_t *peer_addr)
{
    if (!s_inited || !peer_addr) return false;
    uint8_t exists = 0;
    if (rpc(ESP_NOW_HOSTED_OP_IS_PEER_EXIST, peer_addr, 6, &exists, 1) != ESP_OK)
        return mirror_find(peer_addr) >= 0;            /* transport trouble: best effort */
    return exists != 0;
}

esp_err_t esp_now_get_peer(const uint8_t *peer_addr, esp_now_peer_info_t *peer)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!peer_addr || !peer) return ESP_ERR_ESPNOW_ARG;
    int i = mirror_find(peer_addr);
    if (i < 0) return ESP_ERR_ESPNOW_NOT_FOUND;
    *peer = s_peers[i];
    return ESP_OK;
}

esp_err_t esp_now_fetch_peer(bool from_head, esp_now_peer_info_t *peer)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!peer) return ESP_ERR_ESPNOW_ARG;
    if (from_head) s_fetch_idx = 0;
    for (; s_fetch_idx < MAX_PEERS; s_fetch_idx++) {
        if (s_peer_used[s_fetch_idx] && !(s_peers[s_fetch_idx].peer_addr[0] & 0x01)) {  /* skip broadcast/multicast, as native */
            *peer = s_peers[s_fetch_idx++];
            return ESP_OK;
        }
    }
    return ESP_ERR_ESPNOW_NOT_FOUND;
}

esp_err_t esp_now_get_peer_num(esp_now_peer_num_t *num)
{
    if (!s_inited) return ESP_ERR_ESPNOW_NOT_INIT;
    if (!num) return ESP_ERR_ESPNOW_ARG;
    num->total_num = num->encrypt_num = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!s_peer_used[i]) continue;
        num->total_num++;
        if (s_peers[i].encrypt) num->encrypt_num++;
    }
    return ESP_OK;
}

esp_err_t esp_now_set_pmk(const uint8_t *pmk)
{
    if (!pmk) return ESP_ERR_ESPNOW_ARG;
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_SET_PMK, pmk, ESP_NOW_KEY_LEN, NULL, 0);
    if (err == ESP_OK) { memcpy(s_pmk, pmk, ESP_NOW_KEY_LEN); s_pmk_set = true; }
    return err;
}

/* Not forwarded by the co-processor overlay (no opcode). Defined so applications
 * link; they fail loudly at runtime instead of with "undefined reference". */
esp_err_t esp_now_set_peer_rate_config(const uint8_t *peer_addr, esp_now_rate_config_t *config)
{ (void)peer_addr; (void)config; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_set_wake_window(uint16_t window) { (void)window; return ESP_ERR_NOT_SUPPORTED; }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
esp_err_t esp_now_set_user_oui(uint8_t *oui) { (void)oui; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_get_user_oui(uint8_t *oui) { (void)oui; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_switch_channel_tx(esp_now_switch_channel_t *config) { (void)config; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_now_remain_on_channel(esp_now_remain_on_channel_t *config) { (void)config; return ESP_ERR_NOT_SUPPORTED; }
#endif

/* ------------------------------------------------------------- extensions -- */
void esp_now_hosted_host_get_stats(esp_now_hosted_host_stats_t *out)
{
    if (out) *out = s_stats;
}

esp_err_t esp_now_hosted_host_reinit(void)
{
    esp_err_t err = rpc(ESP_NOW_HOSTED_OP_INIT, NULL, 0, NULL, 0);
    if (err != ESP_OK) return err;
    s_inited = true;
    if (s_pmk_set) {
        err = rpc(ESP_NOW_HOSTED_OP_SET_PMK, s_pmk, ESP_NOW_KEY_LEN, NULL, 0);
        if (err != ESP_OK) return err;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!s_peer_used[i]) continue;
        esp_now_hosted_peer_t w;
        peer_to_wire(&s_peers[i], &w);
        esp_err_t e = rpc(ESP_NOW_HOSTED_OP_ADD_PEER, &w, sizeof(w), NULL, 0);
        if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) err = e;
    }
    return err;
}

bool esp_now_hosted_host_cp_supported(void)
{
    uint32_t v = 0;
    (void)rpc(ESP_NOW_HOSTED_OP_GET_VERSION, NULL, 0, &v, sizeof(v));
    return s_last_had_resp;   /* any matching reply proves the overlay; stock firmware never answers */
}

esp_err_t esp_now_hosted_host_cp_update(const uint8_t *image, size_t len)
{
    if (!image || len < 256 || image[0] != 0xE9) return ESP_ERR_INVALID_ARG;   /* ESP image magic */
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) return err;
    const size_t chunk = 1400;
    for (size_t off = 0; off < len && err == ESP_OK; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        err = esp_hosted_slave_ota_write((uint8_t *)image + off, (uint32_t)n);
    }
    if (err == ESP_OK) err = esp_hosted_slave_ota_end();
    if (err == ESP_OK) err = esp_hosted_slave_ota_activate();
    if (err == ESP_OK) ESP_LOGW(TAG, "co-processor firmware updated (%u bytes): restart the host now", (unsigned)len);
    else ESP_LOGE(TAG, "co-processor OTA failed: %s", esp_err_to_name(err));
    return err;
}
