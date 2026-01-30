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

// Tick period in milliseconds
#define portTICK_PERIOD_MS 1

// Convert milliseconds to ticks
#define pdMS_TO_TICKS(ms) (ms)

// Boolean values
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

// Base types
typedef long BaseType_t;

// Mock implementation - returns current tick count
extern uint32_t xTaskGetTickCount(void);

#endif // FREERTOS_H
