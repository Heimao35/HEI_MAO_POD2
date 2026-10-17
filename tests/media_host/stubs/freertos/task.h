#pragma once
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task);
