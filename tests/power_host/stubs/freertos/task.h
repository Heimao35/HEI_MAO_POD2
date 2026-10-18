#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
#define pdFALSE 0
#define portYIELD_FROM_ISR() ((void)0)
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task);

void xTaskNotifyGive(TaskHandle_t task);

void vTaskDelay(unsigned ticks);
TickType_t xTaskGetTickCount(void);
