/* Run actual LVGL/player regressions through the production PSRAM allocator.
 * The host heap records capability flags; it does not model P4 bus speed. */
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
typedef union {struct {size_t size;unsigned magic;} h;long double align;} header_t;
static size_t live_bytes,live_blocks,peak_bytes,allocations;
static bool exhaust_psram;
static const unsigned memory_magic=0x37e5abcd;
void *heap_caps_malloc(size_t bytes,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ++allocations;if(exhaust_psram)return NULL;
    header_t *h=malloc(sizeof(*h)+bytes);assert(h);
    h->h.size=bytes;h->h.magic=memory_magic;
    live_bytes+=bytes;++live_blocks;if(live_bytes>peak_bytes)peak_bytes=live_bytes;
    return h+1;
}
void heap_caps_free(void *p)
{
    if(!p)return;
    header_t *h=(header_t*)p-1;assert(h->h.magic==memory_magic);
    live_bytes-=h->h.size;--live_blocks;h->h.magic=0;free(h);
}
void *heap_caps_realloc(void *p,size_t bytes,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!bytes){heap_caps_free(p);return NULL;}
    if(!p)return heap_caps_malloc(bytes,caps);
    header_t *h=(header_t*)p-1;assert(h->h.magic==memory_magic);
    size_t size=h->h.size;void *next=heap_caps_malloc(bytes,caps);
    if(!next)return NULL;
    memcpy(next,p,size<bytes?size:bytes);heap_caps_free(p);return next;
}
void heap_caps_get_info(multi_heap_info_t *info,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    *info=(multi_heap_info_t){.total_allocated_bytes=live_bytes,
        .total_free_bytes=32*1024*1024-live_bytes,.largest_free_block=32*1024*1024-live_bytes,
        .allocated_blocks=live_blocks,.free_blocks=1};
}
bool heap_caps_check_integrity(unsigned caps,bool print_errors)
{assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) && !print_errors);return true;}
#include "../../main/ui/ui_memory.c"
#define main player_regressions
#include "test_player.c"
#undef main
int main(void)
{
    /* A failed grow must preserve the old PSRAM block, never fall back to SRAM. */
    uint8_t *block=lv_malloc_core(16);memset(block,0x5a,16);
    exhaust_psram=true;
    assert(!lv_malloc_core(64) && !lv_realloc_core(block,128));
    for(unsigned i=0;i<16;++i)assert(block[i]==0x5a);
    exhaust_psram=false;block=lv_realloc_core(block,128);
    for(unsigned i=0;i<16;++i)assert(block[i]==0x5a);
    lv_free_core(block);assert(!live_bytes && !live_blocks);
    assert(player_regressions()==0); /* original real gestures/animations/clicks */
    for(unsigned pass=0;pass<3;++pass) {
        uint32_t count=512*(pass+1);
        size_t before=allocations;
        ui_music_player_set_library(count,"many songs");
        assert(s_library_count==count && s_pl_item_count==count);
        assert(allocations>before+count*3); /* rows/icons/titles/styles use PSRAM too */
        for(uint32_t j=0;j<count;j+=count-1) {
            assert(lv_obj_has_flag(s_pl_items[j],LV_OBJ_FLAG_CLICKABLE));
            assert(lv_obj_get_style_height(s_pl_items[j],0)==48);
            assert(playlist_item_icon(s_pl_items[j]) && playlist_item_title(s_pl_items[j]));
        }
        music_page_t page={.first=count-8,.count=8};
        for(unsigned j=0;j<8;++j)snprintf(page.names[j],MUSIC_NAME_MAX,"track %lu",(unsigned long)(page.first+j));
        ui_music_player_set_page(&page);
        assert(!strcmp(lv_label_get_text(playlist_item_title(s_pl_items[count-1])),page.names[7]));
        unsigned requested=requests;
        lv_obj_send_event(s_pl_items[count-1],LV_EVENT_CLICKED,NULL);
        assert(s_current_idx==(int)count-1 && requests==requested+1);
        assert(lv_obj_has_flag(s_playlist,LV_OBJ_FLAG_SCROLLABLE));
        ui_music_player_set_library(0,"empty");
        assert(!s_pl_items && !s_pl_item_count);
    }
    lv_mem_monitor_t mon;lv_mem_monitor_core(&mon);
    assert(mon.total_size==32*1024*1024 && mon.used_cnt==live_blocks);
    assert(lv_mem_test_core()==LV_RESULT_OK);
    printf("PASS real player gestures +512/1024/1536 rows via PSRAM only; peak=%luB; failed grow preserves data\n",(unsigned long)peak_bytes);
    return 0;
}
