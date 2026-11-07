/**
 * Mock task.h for host-based testing
 */

#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack_depth,
                       void *param, BaseType_t priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);

#endif // TASK_H
