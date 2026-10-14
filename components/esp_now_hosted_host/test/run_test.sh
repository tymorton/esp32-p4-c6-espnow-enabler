#!/usr/bin/env bash
# Host unit test for esp_now_hosted_host (no ESP-IDF needed).
# Needs: gcc, git. Fetches esp-wifi-remote (Apache-2.0) for the real injected esp_now.h / wifi types.
set -euo pipefail
cd "$(dirname "$0")"
EWR=${EWR:-build/esp-wifi-remote}
[ -d "$EWR" ] || git clone -q --depth 1 https://github.com/espressif/esp-wifi-remote.git "$EWR"
INJ55="$EWR/components/esp_wifi_remote/idf_v5.5/include/injected"
INJ54="$EWR/components/esp_wifi_remote/idf_v5.4/include/injected"
mkdir -p build
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -pthread -Istubs -I../include"
# IDF 5.4 header set (old send-callback signature): compile-only
gcc $CFLAGS -DTEST_IDF_MINOR=4 -I"$INJ54" -c ../esp_now_hosted_host.c -o build/idf54.o
# IDF 5.5 header set: full test under ASan/UBSan
gcc $CFLAGS -DTEST_IDF_MINOR=5 -I"$INJ55" -fsanitize=address,undefined -fno-omit-frame-pointer \
    ../esp_now_hosted_host.c test_main.c -o build/test_espnow_hosted
./build/test_espnow_hosted
