#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_hosted_send_custom_data(uint32_t msg_id, const uint8_t *data, size_t len);
esp_err_t esp_hosted_register_custom_callback(uint32_t msg_id,
    void (*cb)(uint32_t, const uint8_t *, size_t, void *), void *ctx);
