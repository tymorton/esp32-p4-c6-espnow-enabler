idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hosted esp_wifi_remote esp_now_hosted_host esp_wifi esp_netif nvs_flash
)
