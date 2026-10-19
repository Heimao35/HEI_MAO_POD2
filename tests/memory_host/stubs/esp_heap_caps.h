#pragma once
#include <stddef.h>
#include <stdbool.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct {
    size_t total_free_bytes,total_allocated_bytes,largest_free_block;
    size_t free_blocks,allocated_blocks;
} multi_heap_info_t;
void *heap_caps_malloc(size_t bytes,unsigned caps);
void *heap_caps_realloc(void *ptr,size_t bytes,unsigned caps);
void heap_caps_free(void *ptr);
void heap_caps_get_info(multi_heap_info_t *info,unsigned caps);
bool heap_caps_check_integrity(unsigned caps,bool print_errors);
