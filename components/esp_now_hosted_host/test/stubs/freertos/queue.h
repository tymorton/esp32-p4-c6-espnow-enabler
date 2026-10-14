#pragma once
#include <stddef.h>
#include "FreeRTOS.h"
typedef struct queue_s *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned len, size_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ms);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ms);
