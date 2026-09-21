#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(value) assert(value)
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define BIT3 8u
#define BIT4 16u
#define BIT5 32u
