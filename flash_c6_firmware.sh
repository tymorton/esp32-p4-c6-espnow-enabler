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

echo -e "${YELLOW}Step 1/5: Preparing...${NC}"
cd "$SCRIPT_DIR"

# --- BACKUP SECTION ---
echo ""
echo -e "${YELLOW}Would you like to BACKUP your current P4 firmware before flashing? (y/n)${NC}"
read -r response
if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
    echo -e "${YELLOW}Backing up 4MB from address 0x10000 to 'backup_app.bin'...${NC}"
    python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset read_flash 0x10000 0x400000 backup_app.bin
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Backup successful! Saved to backup_app.bin${NC}"
        HAS_BACKUP=true
    else
        echo -e "${RED}Backup failed! Continuing without backup...${NC}"
        HAS_BACKUP=false
    fi
else
    echo "Skipping backup."
    HAS_BACKUP=false
fi
echo ""
# ----------------------

echo -e "${YELLOW}Step 2/5: Flashing storage partition (0x410000)...${NC}"
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash 0x410000 binaries/storage.bin

echo -e "${YELLOW}Step 3/5: Flashing OTA host firmware...${NC}"
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash \
    0x2000 binaries/bootloader.bin \
    0x8000 binaries/partition-table.bin \
    0xd000 binaries/ota_data_initial.bin \
    0x10000 modified_ota_host/host_performs_slave_ota.bin

echo ""
echo -e "${GREEN}✅ Flash complete!${NC}"
echo ""
echo -e "${YELLOW}Step 4/5: Press RESET button on your board${NC}"
echo ""
echo "Expected output:"
echo "  - FORCING Slave OTA update"
echo "  - Using LittleFS OTA method"
echo "  - Slave firmware version: 2.6.7"
echo ""
echo -e "${GREEN}ESP-NOW is now enabled on your ESP32-C6!${NC}"

# --- RESTORE SECTION ---
if [ "$HAS_BACKUP" = true ]; then
    echo ""
    echo -e "${YELLOW}Step 5/5: Restore Backup${NC}"
    echo "Do you want to RESTORE your original firmware now? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        echo -e "${YELLOW}Restoring 'backup_app.bin' to 0x10000...${NC}"
        python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash 0x10000 backup_app.bin
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}Restore successful! Your original app is back.${NC}"
        else
            echo -e "${RED}Restore failed! You may need to re-flash your original app manually.${NC}"
        fi
    else
        echo "Keeping OTA host firmware. You can restore later using:"
        echo "esptool.py --chip esp32p4 -p $PORT write_flash 0x10000 backup_app.bin"
    fi
fi
# -----------------------
