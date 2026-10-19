#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
/* Media host tests are single-threaded; firmware uses the real SMP spinlock. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)

#define portTICK_PERIOD_MS 1
