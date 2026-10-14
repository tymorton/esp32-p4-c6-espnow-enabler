#pragma once
#include "FreeRTOS.h"
typedef struct sem_s *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ms);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
