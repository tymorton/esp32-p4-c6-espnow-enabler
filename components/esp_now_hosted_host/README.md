# esp_now_hosted_host

`esp_now_init()`, `esp_now_send()` and the rest of the native ESP-NOW API on an **ESP32-P4**, whose radio lives on an ESP32-C6 (or C5/C61) co-processor connected through **esp_hosted**.

Espressif's esp_hosted forwards `esp_wifi_*` calls but **not** `esp_now_*` (open issue [esp-hosted-mcu#19](https://github.com/espressif/esp-hosted-mcu/issues/19)). On a P4, `esp_wifi_remote` ships the `esp_now.h` header with no implementation, so code links with `undefined reference to esp_now_init`. This component supplies those functions and forwards them over esp_hosted's CustomRpc ("peer data transfer") channel. On the co-processor, firmware built with ESPHome's **ESP-NOW overlay** turns them into native calls.

```
P4 app ── esp_now_send() ─► this component ─► esp_hosted CustomRpc (SDIO) ─► C6 overlay ─► native esp_now_send()
P4 app ◄─ recv/send callbacks ◄─ dispatch task ◄─ RECV/SEND events ◄────────── C6 overlay ◄─ radio
```

| Piece | Where | License |
|---|---|---|
| Host side (this component) | `esp_now_hosted_host.c`, an independent C implementation | Apache-2.0 |
| Wire contract | `include/esp_now_hosted_rpc.h`, a verbatim copy from [esphome/esp-hosted-firmware](https://github.com/esphome/esp-hosted-firmware) `slave-overlay/` @ `e4c39e0` | Apache-2.0 |
| Co-processor firmware | esp_hosted slave/cp + `slave-overlay`, prebuilt by ESPHome or built yourself | Apache-2.0 |

ESPHome's own *host* shim (`esphome/components/esp32_hosted/esp_now_hosted.cpp`) is GPLv3 (the ESPHome C++ runtime license) and is **not** used here. That keeps your firmware free of a GPL obligation.

## Use

1. **Co-processor firmware with the overlay.** Pick one:
   - Prebuilt: [esphome.github.io/esp-hosted-firmware](https://esphome.github.io/esp-hosted-firmware/) (manifest `manifest/esp32c6.json`).
   - Build it yourself:
     - esp_hosted **2.x**: `idf.py create-project-from-example "espressif/esp_hosted==2.12.13:slave"`, then `apply-overlay.sh slave`.
     - esp_hosted **3.x**: copy `slave-overlay/` into the `bleprph_wifi_coex/cp` project's `components/`.

   **The host and co-processor esp_hosted major.minor must match.** A 3.x host needs 3.x co-processor firmware.
2. **Flash the co-processor from your app** (no throw-away OTA app). Embed the binary and call:
   ```c
   extern const uint8_t c6_fw_start[] asm("_binary_network_adapter_bin_start");
   extern const uint8_t c6_fw_end[]   asm("_binary_network_adapter_bin_end");
   if (!esp_now_hosted_host_cp_supported()) {
       ESP_ERROR_CHECK(esp_now_hosted_host_cp_update(c6_fw_start, c6_fw_end - c6_fw_start));
       esp_restart();
   }
   ```
   `esp_now_hosted_host_cp_supported()` sends one probe: stock firmware never answers it, overlay firmware always does. Gate the update on it, or on `esp_hosted_get_coprocessor_fwversion()`, so it runs once.
3. **sdkconfig**:
   - esp_hosted 2.x: `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y` (default y). `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS` must be at least 3 (default 3). Raise it if you use custom data yourself.
   - esp_hosted 3.x: `CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA=y` and `CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_MAX_CUSTOM_MSG_HANDLERS>=3`.
   - The build fails with a clear `#error` if peer data is disabled.
4. Use ESP-NOW exactly as on any ESP32: `esp_wifi_start()` → `esp_now_init()` → `esp_now_add_peer()` → `esp_now_send()`.

## Behavior vs native ESP-NOW

- **Callbacks run in this component's dispatch task.** Calling `esp_now_send()` from inside them is safe. A test covers it: it would deadlock if callbacks ran on esp_hosted's RX thread.
- **`esp_now_send()` returns once the co-processor queues the frame** (≈ one SDIO round trip). The TX result arrives in the send callback, as natively.
- **Payloads up to 1470 bytes (ESP-NOW v2).** Peers running v1 only accept ≤ 250 bytes.
- **`get_peer` / `fetch_peer` / `get_peer_num` answer from a local mirror** of peers added through this API.
- **`rx_ctrl` fields:** `rssi` and `channel` come from the co-processor. `timestamp` is the host receive time (µs), not the radio timestamp.
- **Not forwarded** (return `ESP_ERR_NOT_SUPPORTED`): `set_peer_rate_config`, `set_wake_window`, `set/get_user_oui`, `switch_channel_tx`, `remain_on_channel`. `esp_wifi_config_espnow_rate` is not provided.
- **Co-processor reset:** ESP-NOW state on the C6 is lost. Call `esp_now_hosted_host_reinit()`, which re-inits and re-adds the PMK and peers.
- **Stats:** `esp_now_hosted_host_get_stats()` reports calls, timeouts, stale responses, RX frames, drops, and TX events.

## Tests

`test/run_test.sh` builds the real component source against:
- the real `esp_wifi_remote` injected headers (IDF 5.4 and 5.5 variants),
- a FreeRTOS-on-pthreads shim,
- a fake co-processor that follows the overlay's protocol, with fault injection.

It runs under AddressSanitizer and UBSan. It covers:
- detecting stock firmware
- init, peer and PMK handling
- a 1470-byte round trip with RSSI and source address
- re-entrant send from inside a callback
- a late response not being confused with the next request
- co-processor reboot and reinit
- deinit
- the co-processor OTA helper
- unsupported APIs

**Not yet run on hardware.** Validate on a P4 + C6 board before relying on it (see `specs/proposed/SYSTEM_SPEC.md` §7.4, Phase 0).
