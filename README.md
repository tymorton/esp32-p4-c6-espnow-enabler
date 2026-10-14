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

### 2. Why We Pin to `2.12.x`
The `esp_hosted` 2.12.x branch is pinned because it is the validated LTS release where the CustomRpc ESP-NOW bridge has been thoroughly tested on physical silicon. While newer `esp_wifi_remote` (>=1.6.5) relaxed its upper version limit, `esp_hosted` major versions cannot be mixed across the SDIO link (a 3.x host cannot talk to a 2.x slave). 

To prevent IDF component manager from resolving an incompatible 3.x host driver on fresh projects, `components/esp_now_hosted_host/idf_component.yml` explicitly requires `espressif/esp_hosted: ">=2.8.1,<3.0"`.

### 3. Tested & Verified Compatibility Matrix

| Component | Tested Version | Status / Notes |
| :--- | :--- | :--- |
| **ESP-IDF** | `v5.4.x`, `v5.5.5` | Recommended modern toolchain (GCC 14.2.0) |
| **Host `esp_hosted`** | `2.12.12` | Pinned to `2.x` in component manifest |
| **Host `esp_wifi_remote`** | `1.6.3` / `1.6.5` | Standard remote Wi-Fi layer |
| **C6 Co-Processor Firmware**| `2.12.13` | Provided in `slave_firmware/network_adapter.bin` |
| **Host ESP-NOW Component** | `esp_now_hosted_host` | Included in `components/esp_now_hosted_host` |
| **LVGL (Optional Display)** | `v9.6.0` | 60 FPS on 10.1" / 7" MIPI-DSI displays |

---

## 📱 Hardware Support

### ✅ Tested on Physical Hardware
- **Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1** (10.1" MIPI-DSI, SDIO slot 1, CLK 18, CMD 19, D0–D3 14–17, reset 54) verified bidirectional communication with an ESP32-S3 peer on Channel 1.

### 🟡 Expected to Work (Same or Compatible Pinout)
- **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** (7" Display)
- **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** (4" Round Display)
- **Waveshare ESP32-P4-Module-DEV-KIT**
- **Waveshare ESP32-P4-NANO**
- **Elecrow CrowPanel ESP32-P4 7.0" HMI**

> [!NOTE]
> Non-Waveshare boards may assign different GPIOs for SDIO lines or C6 reset. Check your board's schematic and configure `CONFIG_ESP_HOSTED_SDIO_PIN_*` and `CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE` in `menuconfig` accordingly.

---

## 🚀 Quick Start

You can update the C6 co-processor either **directly from your P4 application** (recommended) or using the **standalone flasher script** (fallback).

---

### Option A: Direct In-App Co-Processor Update (Recommended)

No external scripts or partition overwrites required! Copy `slave_firmware/network_adapter.bin` into your project's `main/` directory and add it to `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hosted esp_wifi_remote esp_now_hosted_host esp_wifi esp_netif nvs_flash
    EMBED_FILES "network_adapter.bin"
)
```

In your `app_main`, execute the check right after `esp_hosted_connect_to_slave()` and **before** Wi-Fi initialization:

```c
#include "esp_now_hosted_host.h"

// 1. Connect to C6 co-processor
ESP_ERROR_CHECK(esp_hosted_connect_to_slave());

// 2. Check if co-processor has ESP-NOW overlay. If not, self-update and restart:
if (!esp_now_hosted_host_cp_supported()) {
    extern const uint8_t c6_bin_start[] asm("_binary_network_adapter_bin_start");
    extern const uint8_t c6_bin_end[]   asm("_binary_network_adapter_bin_end");
    size_t c6_bin_len = c6_bin_end - c6_bin_start;

    ESP_LOGW("APP", "Flashing ESP-NOW enabled firmware to C6 co-processor...");
    ESP_ERROR_CHECK(esp_now_hosted_host_cp_update(c6_bin_start, c6_bin_len));
    esp_restart();
}
```

> [!NOTE]
> Option A has been verified on boards running existing co-processor firmware (e.g. v2.6.7+). Very old factory co-processor firmware from early production batches may hang on RPC queries and require Option B for the initial upgrade.

---

### Option B: Standalone Flasher Script (Fallback)

> [!WARNING]
> This standalone script temporarily flashes a dedicated OTA host image that overwrites the P4 bootloader and partition table. Back up your full flash beforehand if you have existing partition data or NVS keys. The OTA tool also forces a C6 rewrite on every boot until you restore your application.

Connect your ESP32-P4 board via USB and run:

```bash
git clone https://github.com/tymorton/esp32-p4-c6-espnow-enabler.git
cd esp32-p4-c6-espnow-enabler
chmod +x flash_c6_firmware.sh
./flash_c6_firmware.sh /dev/cu.usbmodemXXXX   # (Linux: /dev/ttyACM0 or /dev/ttyUSB0)
```

---

### 📦 Integration in Your P4 Project

#### 1. Add the Component to Your Project

Copy `components/esp_now_hosted_host` into your project's `components/` directory:

```bash
cp -r esp32-p4-c6-espnow-enabler/components/esp_now_hosted_host /path/to/your/p4_project/components/
```

In your `CMakeLists.txt`, add `esp_now_hosted_host` to `REQUIRES`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hosted esp_wifi_remote esp_now_hosted_host esp_wifi esp_netif nvs_flash
)
```

#### 2. Complete Working Code Example

*(See [`examples/ping_pong/`](examples/ping_pong/) for the P4 project, and [`examples/ping_pong_peer/`](examples/ping_pong_peer/) for an ESP32/S3/C3/C6 responder peer)*

```c
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
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
    // 1. Initialize NVS (required by Wi-Fi stack)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Connect P4 to C6 coprocessor over SDIO
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Connected to C6 over SDIO!");

    // 3. Initialize Wi-Fi stack via esp_wifi_remote
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "Wi-Fi started on Channel 1");

    // 4. Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGI(TAG, "✅ ESP-NOW initialized on P4 via C6 CustomRpc!");

    // 5. Register Callbacks (run on dedicated dispatch worker task)
    esp_now_register_recv_cb(on_espnow_recv);
    esp_now_register_send_cb(on_espnow_send);

    // 6. Add Peer (Unicast or Broadcast)
    esp_now_peer_info_t peer = {
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, TARGET_PEER_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    // 7. Transmit Data
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
