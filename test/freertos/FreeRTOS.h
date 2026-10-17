/**
 * Mock FreeRTOS.h for host-based testing
 *
 * This provides minimal type definitions and macros needed for testing
 * without requiring the actual FreeRTOS library.
 */

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>

// FreeRTOS tick type
typedef uint32_t TickType_t;

// Most host tests use a 1 kHz clock for convenience. Bus arbitration is also
// exercised at ESP-IDF's default 100 Hz to catch tick-rounding regressions.
#ifdef TEST_FREERTOS_100HZ
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((ms) / portTICK_PERIOD_MS)
#else
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
#endif

// Boolean values
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

// Base types
typedef long BaseType_t;

// Mock implementation - returns current tick count
extern uint32_t xTaskGetTickCount(void);

#endif // FREERTOS_H
