#pragma once
#include <stdio.h>
#define ESP_LOGE(t, f, ...) fprintf(stderr, "E %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) fprintf(stderr, "W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGI(t, f, ...) fprintf(stderr, "I %s: " f "\n", t, ##__VA_ARGS__)
