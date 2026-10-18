#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
static inline void *heap_caps_malloc(size_t bytes, unsigned caps) { (void)caps; return malloc(bytes); }
static inline void heap_caps_free(void *p) { free(p); }
static inline void *heap_caps_aligned_calloc(size_t align, size_t count, size_t bytes, unsigned caps)
{ (void)align; (void)caps; return calloc(count, bytes); }
