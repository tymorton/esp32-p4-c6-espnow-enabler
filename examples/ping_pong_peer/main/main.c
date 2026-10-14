#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espnow_peer";

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "RECV from %02X:%02X:%02X:%02X:%02X:%02X | len=%d | msg='%.*s'",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5],
             len, len, (char *)data);

    // If incoming message contains "PING", reply with "PONG"
    if (len >= 4 && memcmp(data, "PING", 4) == 0) {
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peer = {
                .channel = 1,
                .ifidx = WIFI_IF_STA,
                .encrypt = false,
            };
            memcpy(peer.peer_addr, info->src_addr, ESP_NOW_ETH_ALEN);
            esp_now_add_peer(&peer);
        }

        const char *reply = "PONG";
        esp_err_t err = esp_now_send(info->src_addr, (const uint8_t *)reply, strlen(reply));
        ESP_LOGI(TAG, "PONG reply send triggered: %s", esp_err_to_name(err));
    }
}

static void on_espnow_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "SEND status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS (ACK received)" : "FAIL");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP-NOW Responder Peer (ESP32 / S3 / C3 / C6)");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Initialize Wi-Fi in Station mode on Channel 1
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
    ESP_LOGI(TAG, "ESP-NOW initialized");

    // 4. Register Callbacks
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_espnow_send));

    ESP_LOGI(TAG, "Listening for PING packets from ESP32-P4...");
}
