#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4_ping_pong";

// Broadcast address to discover any ESP-NOW peer on the same channel
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "RECV from %02X:%02X:%02X:%02X:%02X:%02X | len=%d | data='%.*s'",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5],
             len, len, (char *)data);

    // If we received a PING, respond with PONG
    if (len >= 4 && memcmp(data, "PING", 4) == 0) {
        // ESP-NOW requires peer registration before unicast transmission
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peer = {
                .channel = 1,
                .ifidx = WIFI_IF_STA,
                .encrypt = false,
            };
            memcpy(peer.peer_addr, info->src_addr, ESP_NOW_ETH_ALEN);
            esp_now_add_peer(&peer);
        }

        // Safe to call esp_now_send inside callback thanks to async dispatch task!
        const char *pong_reply = "PONG";
        esp_now_send(info->src_addr, (const uint8_t *)pong_reply, strlen(pong_reply));
        ESP_LOGI(TAG, "Sent PONG response to peer");
    }
}

static void on_espnow_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "SEND status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS (ACK received)" : "FAIL / NO ACK");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-P4 ESP-NOW Ping-Pong Demo");

    // 1. Initialize NVS (required by Wi-Fi stack)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Connect P4 to C6 co-processor over SDIO
    ESP_LOGI(TAG, "Connecting to ESP32-C6 over SDIO...");
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "SDIO link synchronized!");

    // 3. Initialize Wi-Fi stack via esp_wifi_remote
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "Wi-Fi started in STA mode on Channel 1");

    // 4. Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGI(TAG, "✅ ESP-NOW initialized on P4 via C6 CustomRpc!");

    // 5. Register Callbacks
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_espnow_send));

    // 6. Register Broadcast Peer
    esp_now_peer_info_t peer = {
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "Broadcast peer registered");

    // 7. Periodic Ping Loop
    uint32_t count = 0;
    while (1) {
        char msg[32];
        snprintf(msg, sizeof(msg), "PING #%lu", (unsigned long)++count);
        ESP_LOGI(TAG, "Broadcasting: '%s' ...", msg);
        esp_now_send(BROADCAST_MAC, (const uint8_t *)msg, strlen(msg));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
