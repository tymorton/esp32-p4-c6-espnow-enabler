# ESP32-P4 C6 ESP-NOW Enabler

**Enable bidirectional ESP-NOW communication on ESP32-P4 boards (Waveshare, Elecrow, etc.) with onboard ESP32-C6 Wi-Fi 6 coprocessors via SDIO CustomRpc.**

---

## 🎯 The Root Cause: Why Stock P4 Boards Cannot Run ESP-NOW

If you have tried using ESP-NOW on an ESP32-P4 with an ESP32-C6 coprocessor, you likely encountered one or both of these roadblocks:

1. **Host Linker Error (`undefined reference to esp_now_init`)**:  
   Espressif's `esp_wifi_remote` component injects the header `<esp_now.h>` on the P4, but ships **no implementation (`.c` file)** at link time ([tracking issue espressif/esp-hosted-mcu#19](https://github.com/espressif/esp-hosted-mcu/issues/19)).
2. **Coprocessor Lacks RPC Handlers**:  
   Stock factory C6 firmware from Espressif has **no CustomRpc handler** for ESP-NOW. Even if you stub the host symbols, no packets cross the SDIO bus.

### The Solution Provided Here

This repository provides the complete, tested end-to-end fix:
- **Coprocessor Firmware (`slave_firmware/network_adapter.bin`)**: ESP-Hosted v2.12.13 compiled with the `esp_now_hosted` CustomRpc overlay (RPC ID 388 / `0x6E6F7701`).
- **Drop-In Host Component (`components/esp_now_hosted_host`)**: A clean C component (Apache-2.0) with an asynchronous event dispatch task that provides the full `<esp_now.h>` API on the P4, routing all calls over SDIO CustomRpc to the C6 coprocessor without blocking or deadlocking callbacks.
- **Automated Flasher (`flash_c6_firmware.sh`)**: Updates the onboard C6 over SDIO without requiring external JTAG or UART wiring.

---

## ⚠️ Version Compatibility & Requirements (Crucial)

Before building or flashing, please note the version pairing rules:

### 1. Host and Co-Processor Major Versions Must Match
`esp_hosted` uses an internal RPC protocol across SDIO. A **v2.x host** can only communicate with a **v2.x co-processor**, and a **v3.x host** can only communicate with a **v3.x co-processor**. Mixing major versions breaks the SDIO transport handshake immediately.

### 2. Why We Pin to `2.12.x` (and NOT `3.0.x`)
Espressif maintains two active branches of `esp_hosted`:
- **`2.12.x` (LTS / Stable Production)**: The official standard for MCU-to-MCU hosted mode.
- **`3.0.x` (Experimental Redesign)**: New transport protocol.

In ESP-IDF v5.4 and v5.5, Espressif's own `esp_wifi_remote` component (which provides the Wi-Fi stack for the ESP32-P4) **explicitly restricts** `esp_hosted` for P4 targets:
```yaml
# From Espressif's esp_wifi_remote component manifest:
rules:
  - if: target in [esp32h2, esp32p4]
    version: ">=2.11,<3.0"
```
Because Espressif blocks `3.0.x` on the P4 host, **the C6 co-processor must also run the matching `2.12.x` release**. Upgrading the C6 to a 3.0.x binary will cause `Req_CustomRpc` timeouts `[0x184]`.

### 3. Tested & Verified Compatibility Matrix

| Component | Tested Version | Status / Notes |
| :--- | :--- | :--- |
| **ESP-IDF** | `v5.4.x`, `v5.5.5` | Recommended modern toolchain (GCC 14.2.0) |
| **Host `esp_hosted`** | `2.12.12` | Specified in `main/idf_component.yml` |
| **Host `esp_wifi_remote`** | `1.6.3` | Standard remote Wi-Fi layer |
| **C6 Co-Processor Firmware**| `2.12.13` | Provided in `slave_firmware/network_adapter.bin` |
| **Host ESP-NOW Component** | `esp_now_hosted_host` | Included in `components/esp_now_hosted_host` |
| **LVGL (Optional Display)** | `v9.6.0` | 60 FPS on 10.1" / 7" MIPI-DSI displays |

---

## 📱 Supported Hardware

Verified and tested bidirectional communication on:
- ✅ **Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1** (10.1" MIPI-DSI)
- ✅ **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** (7" Display)
- ✅ **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** (4" Round Display)
- ✅ **Waveshare ESP32-P4-Module-DEV-KIT**
- ✅ **Waveshare ESP32-P4-NANO**
- ✅ **Elecrow CrowPanel ESP32-P4 7.0" HMI**
- ✅ Any ESP32-P4 board with an ESP32-C6 connected via SDIO.

---

## 🚀 Quick Start (3 Steps)

### Step 1: Flash ESP-NOW Firmware to the ESP32-C6 Coprocessor

Connect your ESP32-P4 board via USB and run:

```bash
git clone https://github.com/tymorton/esp32-p4-c6-espnow-enabler.git
cd esp32-p4-c6-espnow-enabler
chmod +x flash_c6_firmware.sh
./flash_c6_firmware.sh /dev/cu.usbmodemXXXX   # (Linux: /dev/ttyACM0 or /dev/ttyUSB0)
```

The script will:
1. Back up your existing P4 app (optional).
2. Flash the SDIO OTA updater to your P4.
3. Automatically stream the ESP-NOW enabled C6 firmware (`network_adapter.bin`) across SDIO into the C6.
4. Prompt you to restore your application.

---

### Step 2: Add the `esp_now_hosted_host` Component to Your P4 Project

Copy the drop-in host component into your project's `components` directory:

```bash
cp -r esp32-p4-c6-espnow-enabler/components/esp_now_hosted_host /path/to/your/p4_project/components/
```

In your main `CMakeLists.txt` or component `CMakeLists.txt`, add `esp_now_hosted_host` to `REQUIRES`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hosted esp_wifi_remote esp_now_hosted_host esp_wifi esp_netif
)
```

*(Optional)* You can also use `esp_now_hosted_host_cp_update(c6_bin, len)` directly in your C application on boot to update the C6 co-processor over SDIO without running any shell script!

---

### Step 3: Initialize Wi-Fi & ESP-NOW in Your Code

> [!IMPORTANT]
> **Prerequisite:** On ESP32 systems, Wi-Fi **must** be initialized and started (`esp_wifi_init()` and `esp_wifi_start()`) before calling `esp_now_init()`.

```c
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_now.h"

static const char *TAG = "p4_espnow";
static const uint8_t TARGET_PEER_MAC[6] = {0x10, 0x20, 0xBA, 0x7A, 0x99, 0xB8};

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "RECV from %02X:%02X:%02X:%02X:%02X:%02X | len=%d",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5], len);
}

static void on_espnow_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "SEND status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. Connect P4 to C6 coprocessor over SDIO
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Connected to C6 over SDIO!");

    // 2. Initialize Wi-Fi stack via esp_wifi_remote
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "Wi-Fi started on Channel 1");

    // 3. Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGI(TAG, "✅ ESP-NOW initialized on P4 via C6 CustomRpc!");

    // 4. Register Callbacks
    esp_now_register_recv_cb(on_espnow_recv);
    esp_now_register_send_cb(on_espnow_send);

    // 5. Add Peer (Unicast or Broadcast)
    esp_now_peer_info_t peer = {
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, TARGET_PEER_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    // 6. Transmit Data
    uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
    esp_now_send(TARGET_PEER_MAC, payload, sizeof(payload));
}
```

---

## 🔧 Frequently Asked Questions & Troubleshooting

### Q1: `undefined reference to esp_now_init` when compiling on P4
**Cause**: `esp_wifi_remote` only provides `<esp_now.h>` declarations without definitions.  
**Fix**: Add the `components/esp_now_hosted_host` folder from this repo into your project's `components/` directory.

### Q2: `Req_CustomRpc` timeout `[0x184]` on boot
**Cause**: The C6 coprocessor is still running factory firmware or mismatched version without the CustomRpc handler.  
**Fix**: Run `./flash_c6_firmware.sh` to install the `network_adapter.bin` v2.12.13 firmware onto the C6.

### Q3: `esp_now_init` fails or packets aren't received
**Cause**: Wi-Fi was not started before calling `esp_now_init()`.  
**Fix**: Ensure `esp_wifi_init(&wcfg)`, `esp_wifi_set_mode(WIFI_MODE_STA)`, and `esp_wifi_start()` are executed before `esp_now_init()`. Both devices must also be configured to the same Wi-Fi channel (`esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)`).

---

## 📄 License & Credits

- Underlying ESP-Hosted framework: Apache 2.0 (Espressif Systems).
- CustomRpc ESP-NOW bridge overlay: Apache 2.0 (ESPHome & contributors).
- Packaged, tested, and maintained for the ESP32-P4 developer community.
