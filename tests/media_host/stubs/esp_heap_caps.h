#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *test_heap_malloc(size_t size, unsigned caps);
void *test_heap_calloc(size_t count, size_t size, unsigned caps);
void test_heap_free(void *ptr);
#define heap_caps_malloc test_heap_malloc
#define heap_caps_calloc test_heap_calloc
#define free test_heap_free
