#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define MALLOC_CAP_CACHE_ALIGNED 16
typedef struct {void *base;size_t size;} test_heap_header_t;
static inline void *test_heap_alloc(size_t a,size_t n)
{
    void *base=malloc(n+a+sizeof(test_heap_header_t));if(!base)return NULL;
    uintptr_t x=((uintptr_t)base+sizeof(test_heap_header_t)+a-1)&~(uintptr_t)(a-1);
    test_heap_header_t *h=(test_heap_header_t*)x-1;h->base=base;h->size=n;
    return (void*)x;
}
static inline void test_heap_free(void *p)
{if(p)free(((test_heap_header_t*)p-1)->base);}
static inline void *test_heap_realloc(void *p,size_t n)
{
    void *next=test_heap_alloc(64,n);if(!next)return NULL;
    if(p){size_t old=((test_heap_header_t*)p-1)->size;memcpy(next,p,old<n?old:n);test_heap_free(p);}
    return next;
}
#define heap_caps_aligned_alloc(a,n,c) test_heap_alloc(a,n)
#define heap_caps_malloc(n,c) test_heap_alloc(64,n)
#define heap_caps_realloc(p,n,c) test_heap_realloc(p,n)
#define heap_caps_free test_heap_free
