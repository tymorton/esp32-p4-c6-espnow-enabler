idf_component_register(
    SRCS          "esp_now_hosted_host.c"
    INCLUDE_DIRS  "include"
    REQUIRES      esp_hosted esp_wifi_remote esp_wifi
    PRIV_REQUIRES esp_timer log
)
