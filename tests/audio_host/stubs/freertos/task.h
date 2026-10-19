#pragma once
#include "freertos/FreeRTOS.h"
#define pdFALSE 0
#define portYIELD_FROM_ISR() ((void)0)
#define taskYIELD() ((void)0)
typedef void *TaskHandle_t;
void vTaskDelay(unsigned ticks);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *task,int core);
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *wake);
