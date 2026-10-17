/* Run the actual worker helpers against deterministic SD/JPEG/UI doubles.
 * Hooks simulate a UI selection or card change while a blocking read is active. */
#include <assert.h>
#include <stdio.h>
#include "../../main/music_player_adapter.c"

static bool ui_locked, sd_locked, mounted = true;
static uint32_t generation = 1, count = 4, ui_index;
static uint32_t decoded, published, shown, lyric_published, cover_before_probe;
static uint8_t covers[4];
static bool cached[4];
static unsigned refs[4];
static int interrupt_index = -1;
static bool interrupt_card;
static uint32_t interrupt_target;
static uint8_t backgrounds[4];
static bool background_cached[4], interrupt_background, missing_cover;
static unsigned background_refs[4];
static uint32_t background_generated, background_published, background_cached_selects;

bool lvgl_port_lock(uint32_t timeout)
{ (void)timeout; assert(!ui_locked && !sd_locked); ui_locked=true; return true; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked=false; }
bool sd_card_is_mounted(void) { return mounted; }
uint32_t sd_card_get_generation(void) { return generation; }
bool sd_card_io_lock(TickType_t ticks)
{ (void)ticks; assert(!ui_locked && !sd_locked); sd_locked=true; return true; }
void sd_card_io_unlock(void) { assert(sd_locked); sd_locked=false; }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{ (void)clear; (void)ticks; return 0; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *task=(void *)1; return pdPASS; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{ (void)semaphore; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { (void)semaphore; return pdTRUE; }

uint32_t music_library_get_count(void) { return count; }
esp_err_t music_library_get_item(uint32_t index, music_item_t *out)
{
    assert(sd_locked && !ui_locked && index<count);
    memset(out,0,sizeof(*out));
    snprintf(out->name,sizeof(out->name),"song%u",index);
    snprintf(out->cover,sizeof(out->cover),"c%u",index);
    snprintf(out->audio,sizeof(out->audio),"a%u",index);
    snprintf(out->lyric,sizeof(out->lyric),"l%u",index);
    return ESP_OK;
}
esp_err_t music_library_make_path(const music_item_t *item, const char *file,
                                  char *out, size_t size)
{ (void)item; snprintf(out,size,"%s",file); return ESP_OK; }
esp_err_t music_library_scan(void) { return ESP_OK; }
void music_library_reset(void) {}
bool music_library_is_truncated(void) { return false; }
esp_err_t music_library_get_page(uint32_t first, music_page_t *page)
{ (void)first; (void)page; return ESP_FAIL; }

const uint8_t *cover_loader_acquire(uint32_t gen, uint32_t index)
{ if (gen!=generation || !cached[index]) return NULL; ++refs[index]; return &covers[index]; }
void cover_loader_retain(const uint8_t *pixels)
{ if (pixels) ++refs[pixels-covers]; }
void cover_loader_release(const uint8_t *pixels)
{ if (pixels) { assert(refs[pixels-covers]); --refs[pixels-covers]; } }
esp_err_t cover_loader_init(void) { return ESP_OK; }
void cover_loader_invalidate(void) { memset(cached,0,sizeof(cached)); }
esp_err_t cover_loader_load(const char *path, uint32_t gen, uint32_t index,
                            const uint8_t **pixels)
{
    (void)path; assert(sd_locked && !ui_locked);
    if (missing_cover) { *pixels=NULL; return ESP_ERR_NOT_FOUND; }
    *pixels=cover_loader_acquire(gen,index);
    if (*pixels) return ESP_OK;
    ++decoded;
    if ((int)index==interrupt_index) {
        interrupt_index=-1;
        if (interrupt_card) ++generation;
        else {
            /* This models another task obtaining the LVGL lock while SD work runs. */
            ui_locked=true; ui_index=interrupt_target;
            select_song(interrupt_target); ui_locked=false;
        }
    }
    cached[index]=true; ++refs[index]; *pixels=&covers[index]; return ESP_OK;
}
esp_err_t music_background_init(void) { return ESP_OK; }
void music_background_invalidate(void) { memset(background_cached,0,sizeof(background_cached)); }
const uint8_t *music_background_acquire(uint32_t gen, uint32_t index)
{
    if (gen!=generation || !background_cached[index]) return NULL;
    ++background_refs[index]; return &backgrounds[index];
}
void music_background_release(const uint8_t *pixels)
{ if (pixels) { assert(background_refs[pixels-backgrounds]); --background_refs[pixels-backgrounds]; } }
esp_err_t music_background_generate(const uint8_t *cover, uint32_t gen, uint32_t index,
                                     const uint8_t **pixels)
{
    assert(cover && !sd_locked && !ui_locked);
    assert(shown && lyric_published); /* Foreground data has priority over image processing. */
    *pixels=music_background_acquire(gen,index);
    if (*pixels) return ESP_OK;
    ++background_generated;
    if (interrupt_background) {
        interrupt_background=false;
        if (interrupt_card) ++generation;
        else {
            ui_locked=true; ui_index=interrupt_target;
            select_song(interrupt_target); ui_locked=false;
        }
    }
    background_cached[index]=true; ++background_refs[index];
    *pixels=&backgrounds[index]; return ESP_OK;
}
void ui_music_player_set_background(uint32_t index, const uint8_t *pixels)
{
    assert(ui_locked && index==ui_index);
    if (pixels) {
        ++background_published;
        if (atomic_load(&s_requested_index)!=UINT32_MAX) ++background_cached_selects;
    }
}
bool ui_music_player_can_show_song(uint32_t index)
{ assert(ui_locked); return index==ui_index; }
void ui_music_player_set_cover(uint32_t index, const uint8_t *pixels)
{ (void)pixels; assert(ui_locked && index==ui_index); ++published; }
void ui_music_player_show_song(uint32_t index, const char *title,
                               const uint8_t *cover, uint32_t duration)
{ (void)title; (void)cover; assert(ui_locked && index==ui_index && duration==123); ++shown; }
void ui_music_player_set_lyrics(const char *text, uint32_t lines, bool truncated)
{ (void)text; (void)lines; (void)truncated; assert(ui_locked); ++lyric_published; }
void ui_music_player_set_library(uint32_t songs, const char *status)
{ (void)songs; (void)status; }
void ui_music_player_set_page(const music_page_t *page) { (void)page; }
void ui_music_player_bind(const ui_music_backend_t *backend) { (void)backend; }
esp_err_t audio_player_probe(const char *path, audio_file_info_t *info)
{
    (void)path; assert(sd_locked && !ui_locked);
    assert(published>cover_before_probe); /* cover publication precedes all audio/LRC I/O */
    info->metadata_known=true; info->duration_ms=123000; return ESP_OK;
}
esp_err_t lyric_parser_load(const char *path) { (void)path; assert(sd_locked); return ESP_OK; }
void lyric_parser_destroy(void) {}
uint32_t lyric_parser_get_line_count(void) { return 1; }
bool lyric_parser_is_truncated(void) { return false; }
size_t lyric_parser_copy_all(char *out, size_t size)
{ if(size) out[0]=0; return 0; }
uint32_t lyric_parser_get_current_line(uint32_t time) { (void)time; return 0; }

static uint32_t choose(uint32_t index)
{
    lvgl_port_lock(0); ui_index=index; select_song(index);
    uint32_t serial=s_request_serial;
    atomic_store(&s_requested_index,UINT32_MAX);
    lvgl_port_unlock(); return serial;
}
static void reset(void)
{
    cover_loader_invalidate(); decoded=published=shown=lyric_published=0;
    cover_before_probe=0; interrupt_index=-1; interrupt_card=false;
    music_background_invalidate(); background_generated=background_published=background_cached_selects=0;
    interrupt_background=missing_cover=false;
    for (unsigned i=0;i<4;i++) assert(background_refs[i]==0);
    for (unsigned i=0;i<4;i++) assert(refs[i]==0);
}
int main(void)
{
    static char text[LYRIC_TEXT_BYTES+1];
    s_lyrics_text=text; s_parser_mutex=(void *)1;
    uint32_t serial=choose(0);
    assert(load_selected_song(0,generation,serial));
    assert(shown==1 && lyric_published==1 && decoded==1);
    prefetch_neighbors(0,generation,serial);
    assert(decoded==3 && cached[0] && cached[1] && cached[3]);
    serial=choose(1); assert(decoded==3); /* select hits cache without decode/I/O */
    assert(load_selected_song(1,generation,serial));
    prefetch_neighbors(1,generation,serial); assert(decoded==4 && cached[2]);
    puts("PASS worker current-first load, early cover publish and adjacent cache reuse");

    reset(); serial=choose(0);
    interrupt_index=0; interrupt_target=2;
    uint32_t before=published;
    assert(!load_selected_song(0,generation,serial));
    assert(shown==0 && lyric_published==0 && published==before+1); /* only the new select */
    assert(atomic_load(&s_requested_index)==2);
    atomic_store(&s_requested_index,UINT32_MAX);
    assert(load_selected_song(2,generation,s_request_serial));
    puts("PASS selection during decode rejects old cover/metadata/lyrics and preserves latest request");

    reset(); serial=choose(0); interrupt_index=1; interrupt_target=2;
    prefetch_neighbors(0,generation,serial);
    assert(decoded==1 && cached[1] && !cached[3]);
    assert(atomic_load(&s_requested_index)==2);
    puts("PASS new selection interrupts remaining prefetch after one in-flight image");

    reset(); count=2; serial=choose(0); prefetch_neighbors(0,generation,serial);
    assert(decoded==1 && cached[1]);
    reset(); count=1; serial=choose(0); prefetch_neighbors(0,generation,serial);
    assert(decoded==0);
    reset(); count=4; serial=choose(0); interrupt_index=0; interrupt_card=true;
    before=published;
    assert(!load_selected_song(0,generation,serial) && published==before && shown==0);
    reset();
    puts("PASS two-song deduplication, single-song library and card generation cancellation");
    serial=choose(0); assert(load_selected_song(0,generation,serial));
    assert(background_generated==1 && background_published==1);
    serial=choose(0);
    assert(background_cached_selects==1 && background_published==2);
    assert(load_selected_song(0,generation,serial));
    assert(background_generated==1); /* Cached background never needs another transform. */
    reset(); serial=choose(0); interrupt_background=true; interrupt_target=2;
    assert(!load_selected_song(0,generation,serial));
    assert(background_generated==1 && !background_published);
    assert(atomic_load(&s_requested_index)==2);
    reset(); serial=choose(0); interrupt_background=true; interrupt_card=true;
    assert(!load_selected_song(0,generation,serial) && !background_published);
    reset(); serial=choose(0); missing_cover=true;
    assert(load_selected_song(0,generation,serial));
    assert(!background_generated && !background_published && shown==1);
    reset();
    puts("PASS background follows foreground, cache hit, selection/card-change rejection and missing cover");
    puts("ALL ADAPTER TESTS PASSED");
    return 0;
}
