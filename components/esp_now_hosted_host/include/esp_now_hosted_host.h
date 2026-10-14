/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_now_hosted_host — native esp_now_* API for radio-less ESP-IDF hosts (ESP32-P4)
 * whose Wi-Fi radio is an esp_hosted co-processor (ESP32-C6/C5/C61).
 *
 * Link this component and call esp_now_init()/esp_now_send()/... exactly as on a
 * chip with a radio. Calls are forwarded over esp_hosted's CustomRpc ("peer data
 * transfer") channel to co-processor firmware that carries the ESP-NOW overlay
 * from github.com/esphome/esp-hosted-firmware (Apache-2.0). The wire contract is
 * include/esp_now_hosted_rpc.h, a verbatim copy of that project's header.
 *
 * Differences from native ESP-NOW (by design):
 *  - Receive/send callbacks run in this component's dispatch task, NOT the Wi-Fi
 *    task. You MAY call esp_now_* from inside them (no deadlock).
 *  - esp_now_send() returns once the co-processor has queued the frame; the TX
 *    result arrives later via the send callback, as natively.
 *  - esp_now_get_peer/fetch_peer/get_peer_num are answered from a local mirror of
 *    peers added through this host (co-processor-side peers added elsewhere are
 *    not visible).
 *  - Not forwarded (return ESP_ERR_NOT_SUPPORTED): set_peer_rate_config,
 *    set_wake_window, set/get_user_oui, switch_channel_tx, remain_on_channel,
 *    esp_wifi_config_espnow_rate.
 *  - If the co-processor resets, its ESP-NOW state is lost: call
 *    esp_now_hosted_host_reinit() (re-runs init and re-adds mirrored peers).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rpc_calls;        /* requests sent */
    uint32_t rpc_timeouts;     /* requests without a matching response in time */
    uint32_t rpc_stale_resp;   /* responses that arrived after their request timed out */
    uint32_t rx_frames;        /* ESP-NOW frames delivered to the app */
    uint32_t rx_dropped;       /* frames dropped: dispatch queue full or no memory */
    uint32_t tx_status_events; /* send-callback events delivered */
} esp_now_hosted_host_stats_t;

/** Snapshot counters (thread-safe, approximate). */
void esp_now_hosted_host_get_stats(esp_now_hosted_host_stats_t *out);

/**
 * Re-establish ESP-NOW on the co-processor after it rebooted (e.g. after
 * esp_hosted reports a transport reset, or after a co-processor OTA): sends INIT,
 * restores the PMK if one was set, and re-adds every peer in the local mirror.
 */
esp_err_t esp_now_hosted_host_reinit(void);

/**
 * True when the co-processor answered the ESP-NOW GET_VERSION request, i.e. it
 * runs firmware with the ESP-NOW overlay. Use at boot to decide whether a
 * co-processor firmware update is required. Does not require esp_now_init().
 */
bool esp_now_hosted_host_cp_supported(void);

/**
 * Optional helper: update the co-processor firmware from a buffer (e.g. a binary
 * embedded with EMBED_FILES or read from a data partition) using esp_hosted's
 * slave-OTA API, then activate it. The host must restart afterwards
 * (esp_hosted re-syncs the transport on boot). Replaces the separate
 * "flash a throw-away OTA host app" step.
 */
esp_err_t esp_now_hosted_host_cp_update(const uint8_t *image, size_t len);

#ifdef __cplusplus
}
#endif
