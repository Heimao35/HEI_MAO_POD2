#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
/* UI对象/样式/标签可随曲库增长，但它们不要求内部DMA内存。原CLIB
 * malloc路径按16384B阈值优先消耗SRAM，歌曲增多时会挤占JPEG描述符。
 * 因此所有LVGL普通分配固定走PSRAM，不回退SRAM；显示驱动、JPEG、
 * I2S需要的DMA缓冲仍由各自heap_caps接口分配，不经过这里。
 * 此文件只更换内存来源，不更改UI对象、布局、事件、动画或绘制格式。 */
#define UI_MEMORY_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{ (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, UI_MEMORY_CAPS); }
void *lv_realloc_core(void *ptr, size_t size)
{ return heap_caps_realloc(ptr, size, UI_MEMORY_CAPS); }
void lv_free_core(void *ptr) { heap_caps_free(ptr); }

/* 与原CLIB backend一样不维护私有内存池。监视器报告共享PSRAM堆，
 * 包含封面/字体等非LVGL用户，不能当作LVGL独占的内存统计。 */
void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, UI_MEMORY_CAPS);
    lv_memzero(mon, sizeof(*mon));
    mon->total_size = info.total_allocated_bytes + info.total_free_bytes;
    mon->free_cnt = info.free_blocks;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_cnt = info.allocated_blocks;
    if (mon->total_size) mon->used_pct = 100 - info.total_free_bytes * 100 / mon->total_size;
    if (mon->free_size) mon->frag_pct = 100 - info.largest_free_block * 100 / mon->free_size;
}
lv_result_t lv_mem_test_core(void)
{ return heap_caps_check_integrity(UI_MEMORY_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID; }
#endif
