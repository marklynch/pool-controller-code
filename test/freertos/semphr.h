/**
 * Mock semphr.h for host-based testing
 *
 * This provides minimal semaphore type definitions and functions needed
 * for testing without requiring the actual FreeRTOS library.
 */

#ifndef SEMPHR_H
#define SEMPHR_H

#include "FreeRTOS.h"

// Opaque semaphore handle type
typedef void* SemaphoreHandle_t;

// Mock semaphore functions
extern BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime);
extern void xSemaphoreGive(SemaphoreHandle_t xSemaphore);

#endif // SEMPHR_H
