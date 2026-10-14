/* Minimal FreeRTOS-on-pthreads shim for host unit tests (only what the component uses). */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7FFFFFFF
typedef struct { pthread_mutex_t m; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { PTHREAD_MUTEX_INITIALIZER }
#define portENTER_CRITICAL(mux) pthread_mutex_lock(&(mux)->m)
#define portEXIT_CRITICAL(mux) pthread_mutex_unlock(&(mux)->m)
