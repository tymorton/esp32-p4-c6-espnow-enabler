#!/bin/bash
# ESP32-P4 C6 ESP-NOW Firmware Flasher
# Usage: ./flash_c6_firmware.sh /dev/cu.wchusbserialXXXX

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PORT=$1

if [ -z "$PORT" ]; then
    echo -e "${RED}Error: Serial port not specified${NC}"
    echo "Usage: $0 /dev/cu.wchusbserialXXXX"
    exit 1
fi

echo -e "${GREEN}=== ESP32-P4 C6 ESP-NOW Firmware Flasher ===${NC}"
echo ""
echo "Port: $PORT"
echo "Firmware: network_adapter.bin (1.1MB, ESP-NOW enabled)"
echo ""

# Check if firmware exists
if [ ! -f "$SCRIPT_DIR/slave_firmware/network_adapter.bin" ]; then
    echo -e "${RED}Error: network_adapter.bin not found!${NC}"
    exit 1
fi

# Check if esptool is available
if ! command -v esptool.py &> /dev/null; then
    echo -e "${RED}Error: esptool.py not found. Please install ESP-IDF.${NC}"
    exit 1
fi

echo -e "${YELLOW}Step 1/4: Preparing...${NC}"
cd "$SCRIPT_DIR"

echo -e "${YELLOW}Step 2/4: Flashing storage partition (0x410000)...${NC}"
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash 0x410000 build/storage.bin

echo -e "${YELLOW}Step 3/4: Flashing OTA host firmware...${NC}"
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash \
    0x2000 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x10000 modified_ota_host/host_performs_slave_ota.bin

echo ""
echo -e "${GREEN}✅ Flash complete!${NC}"
echo ""
echo -e "${YELLOW}Step 4/4: Press RESET button on your board${NC}"
echo ""
echo "Expected output:"
echo "  - FORCING Slave OTA update"
echo "  - Using LittleFS OTA method"
echo "  - Slave firmware version: 2.6.7"
echo ""
echo -e "${GREEN}ESP-NOW is now enabled on your ESP32-C6!${NC}"
