#pragma once
/* Test stub: real esp_wifi_remote type headers, only the one call we use. */
#include "esp_err.h"
#include "esp_wifi_types.h"
esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6]);
