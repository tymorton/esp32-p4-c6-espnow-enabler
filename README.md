# ESP32-P4 C6 ESP-NOW Enabler

**Enable ESP-NOW on ESP32-P4 boards (Elecrow, Waveshare) by updating the ESP32-C6 coprocessor firmware via SDIO OTA.**

## 🎯 Overview

ESP32-P4 boards with ESP32-C6 WiFi coprocessors ship with factory firmware that doesn't include ESP-NOW support. This package provides everything needed to update the C6 firmware to enable ESP-NOW, allowing multiple P4 boards to communicate wirelessly.

### Supported Boards
- ✅ **Elecrow ESP32-P4 7" Display**
- ✅ **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** (4" Round)
- ✅ **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**
- ✅ Any ESP32-P4 board with ESP32-C6 coprocessor via SDIO

## 📦 What's Included

```
esp32-p4-c6-espnow-enabler/
├── README.md                          # This file
├── slave_firmware/
│   └── network_adapter.bin            # ESP-NOW-enabled C6 firmware (1.1MB)
├── modified_ota_host/
│   ├── main_modified.c                # OTA host with version check bypass
│   └── host_performs_slave_ota.bin   # Pre-built OTA flasher
├── docs/
│   ├── modified_version_header.h      # Version 99.99.99 header
│   └── TROUBLESHOOTING.md            # Common issues and solutions
└── flash_c6_firmware.sh              # One-command flash script
```

## 📱 Supported Hardware

This utility is a **Universal Enabler** tested and verified on:

| Board | Status | Notes |
|-------|--------|-------|
| **[Waveshare ESP32-P4-WIFI6-Touch-LCD-4C](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4c.htm)** | ✅ Verified | 4" Round Display |
| **[Elecrow CrowPanel ESP32-P4 7.0" HMI](https://www.elecrow.com/esp32-display-7-inch-hmi-display-rgb-tft-lcd-touch-screen-support-lvgl.html)** | ✅ Verified | 7" Rectangular Display |

*Note: This likely works on ANY ESP32-P4 board with an onboard ESP32-C6 connected via SDIO.*

## 🚀 Quick Start (3 Steps)

### Prerequisites
- ESP-IDF v5.4 or later
- esptool.py installed
- ESP32-P4 board connected via USB

### Method 1: Automated Script (Recommended)

```bash
cd esp32-p4-c6-espnow-enabler
chmod +x flash_c6_firmware.sh
./flash_c6_firmware.sh /dev/cu.wchusbserialXXXX
```

### Method 2: Manual Flash

#### Step 1: Clone ESP-Hosted OTA Example
```bash
git clone --depth 1 https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/examples/host_performs_slave_ota
```

#### Step 2: Configure for LittleFS OTA
```bash
# Copy our ESP-NOW firmware
cp path/to/esp32-p4-c6-espnow-enabler/slave_firmware/network_adapter.bin components/ota_littlefs/slave_fw_bin/

# Apply version check bypass
cp path/to/esp32-p4-c6-espnow-enabler/modified_ota_host/main_modified.c main/main.c

# Configure for LittleFS method
echo "CONFIG_OTA_METHOD_LITTLEFS=y" >> sdkconfig.defaults
echo "CONFIG_OTA_METHOD_PARTITION=n" >> sdkconfig.defaults
```

#### Step 3: Build and Flash
```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

## 📋 Verification

After flashing, you should see:
```
I (2442) host_performs_slave_ota: Slave firmware version: 2.6.7
W (2464) host_performs_slave_ota: FORCING Slave OTA update (version check bypassed)
I (2471) host_performs_slave_ota: Using LittleFS OTA method
```

### Test ESP-NOW is Working

Use this simple test in your P4 application:

```c
#include "esp_now.h"

void test_espnow(void) {
    esp_err_t ret = esp_now_init();
    if (ret == ESP_OK) {
        ESP_LOGI("TEST", "✅ ESP-NOW is working!");
    } else {
        ESP_LOGE("TEST", "❌ ESP-NOW failed: %s", esp_err_to_name(ret));
    }
}
```

## 🔧 Technical Details

### Why This is Needed

1. **Factory C6 Firmware** doesn't include ESP-NOW support
2. **RPC Timeout Issue**: Standard OTA version check fails (RPC 0x15e timeout)
3. **Version Check Bypass**: Our modified OTA forces the update regardless

### What We Changed

#### 1. C6 Firmware Version
Used official version 2.6.7 (unchanged) to ensure future update compatibility.
```c
#define PROJECT_VERSION_MAJOR_1 2
#define PROJECT_VERSION_MINOR_1 6
#define PROJECT_VERSION_PATCH_1 7
```

#### 2. OTA Host Logic
Modified `main.c` to bypass version check:
```c
// FORCE OTA UPDATE - bypass version check due to RPC timeout
ESP_LOGW(TAG, "FORCING Slave OTA update (version check bypassed)");
host_slave_version_not_compatible = true;  // Force OTA
```

### Firmware Details
- **Version**: 2.6.7 (Official)
- **Size**: 1.1MB
- **Transport**: SDIO (10MHz "Safe Mode", 4-bit)
- **Features**: ESP-Hosted slave + ESP-NOW enabled
- **Target**: ESP32-C6
- **✅ Universal Compatibility**: Works on Waveshare, Elecrow, and other P4 boards.
- **✅ Safe Mode**: Uses 10MHz SDIO clock for maximum stability.
- **✅ Smart Flasher**: Auto-detects port, backs up your existing firmware, and restores it after the update.
- **✅ Official Firmware**: Installs Espressif's standard `v2.6.7` firmware.

### Universal Compatibility
To ensure this works on all boards (Waveshare, Elecrow, etc.), we made two key changes:
1.  **10MHz SDIO Clock**: Lowered from 40MHz to 10MHz to ensure robust signal integrity on all PCB layouts.
2.  **Patched File Discovery**: Modified the OTA host to be more lenient when searching for firmware files on LittleFS, fixing "No .bin files found" errors on some devices.

## 🐛 Troubleshooting

### "Slave OTA not required"
**Solution**: Use our modified `main_modified.c` which bypasses the version check.

### "No .bin files found in /littlefs"
**Solution**: Ensure `network_adapter.bin` is in `components/ota_littlefs/slave_fw_bin/` before building.

### "Port is busy"
```bash
lsof | grep /dev/cu.wchusbserial | awk '{print $2}' | xargs kill -9
```

### RPC Timeout (0x15e)
**Normal**: Factory C6 firmware doesn't support version query RPC. Our bypass handles this.

## 📚 Background

This solution was developed through extensive testing on both Elecrow and Waveshare ESP32-P4 boards. We discovered:

1. Factory C6 firmware lacks ESP-NOW
2. ESP-Hosted OTA works but has strict version checks
3. RPC timeouts prevent standard OTA procedures
4. Forcing the OTA bypasses these limitations

## 🤝 Contributing

This package was created to help the ESP32-P4 community. If you:
- Test on other boards
- Find improvements
- Have questions

Please share your findings!

## 📄 License

Based on ESP-Hosted framework (Apache 2.0). Modifications documented for community use.

## ⚠️ Important Notes

- **One-time process**: Once C6 has ESP-NOW firmware, it persists across P4 reflashes
- **Reversible**: Flash factory C6 firmware if needed (contact manufacturer)
- **Safe**: SDIO OTA is the official method for C6 updates

## 🎉 Success Criteria

You'll know it worked when:
1. ✅ C6 reports version 2.6.7
2. ✅ `esp_now_init()` returns `ESP_OK`
3. ✅ No "ESP-NOW not supported" errors
4. ✅ Can send/receive ESP-NOW packets

---

**Created**: December 2024  
**Tested On**: ESP-IDF v5.4, Waveshare 4C, Elecrow 7"  
**Status**: Verified Working ✅
