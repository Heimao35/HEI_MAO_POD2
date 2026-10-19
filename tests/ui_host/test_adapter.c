/* Run the actual worker helpers against deterministic SD/JPEG/UI doubles.
 * Hooks simulate a UI selection or card change while a blocking read is active. */
#include <assert.h>
#include <stdio.h>
#include "../../main/music_player_adapter.c"

/* Real audio hardware lives in its own tested worker; adapter only posts controls. */
static unsigned audio_selects;
static bool dark_screen, eos_ready, repeat_current;
static unsigned dark_eos_calls, dark_eos_taken;
static bool last_autoplay;
void audio_output_select(uint32_t index,uint32_t gen,bool autoplay)
{(void)index;(void)gen;++audio_selects;last_autoplay=autoplay;eos_ready=false;}
void audio_output_toggle(void) {}
void audio_output_set_playing(bool playing) {(void)playing;}
uint32_t audio_output_time_ms(void) {return 0;}
uint32_t audio_output_duration_sec(void) {return 0;}
bool audio_output_is_playing(void) {return false;}
bool audio_output_is_play_requested(void) {return false;}
bool audio_output_is_active(void) {return false;}
bool audio_output_take_finished(void) {return false;}
bool audio_output_quiesce(bool pause) {(void)pause;return true;}
void ui_music_player_set_callbacks(ui_music_ctrl_cb_t p,ui_music_ctrl_cb_t n,ui_music_ctrl_cb_t t,ui_music_ctrl_cb_t m)
{(void)p;(void)n;(void)m;assert(t==audio_output_toggle);}

static bool ui_locked, sd_locked, mounted = true;
bool screen_power_is_dark(void) { return dark_screen; }
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
static TickType_t test_ticks;
static unsigned lock_failures, scan_failures, scan_calls, notify_calls, library_updates;
static unsigned safe_remount_requests;
static uint32_t scan_song_count=4, last_library_count;
static bool replace_card_in_scan;
static char last_library_status[128];
static unsigned boot_anim_starts;
static char saved_folder[MUSIC_NAME_MAX], saved_audio[MUSIC_NAME_MAX];
void player_settings_set_song(const music_item_t *item)
{ assert(ui_locked && !sd_locked); strcpy(saved_folder,item->name); strcpy(saved_audio,item->audio); }
void player_settings_get_song(char *folder, char *audio)
{ assert(ui_locked && !sd_locked); strcpy(folder,saved_folder); strcpy(audio,saved_audio); }
void ui_music_player_restore_selection(uint32_t index)
{ assert(ui_locked); ui_index=index; }
bool ui_lockscreen_is_visible(void) { return true; }
void ui_lockscreen_start_boot_anim(void) { ++boot_anim_starts; }
void test_set_inserted(bool present);
TickType_t xTaskGetTickCount(void) { return test_ticks; }

bool lvgl_port_lock(uint32_t timeout)
{ (void)timeout; assert(!ui_locked && !sd_locked); ui_locked=true; return true; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked=false; }
bool sd_card_is_mounted(void) { return mounted; }
uint32_t sd_card_get_generation(void) { return generation; }
bool sd_card_io_lock(TickType_t ticks)
{
    assert(!ui_locked && !sd_locked);
    if (lock_failures) { --lock_failures; test_ticks+=ticks; return false; }
    sd_locked=true; return true;
}
void sd_card_io_unlock(void) { assert(sd_locked); sd_locked=false; }
void sd_card_request_safe_remount(void) { ++safe_remount_requests; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task); ++notify_calls; }
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
esp_err_t music_library_scan(void)
{
    assert(sd_locked && !ui_locked);
    ++scan_calls;
    if (replace_card_in_scan) { replace_card_in_scan=false; ++generation; }
    if (scan_failures) { --scan_failures; count=0; return ESP_FAIL; }
    count=scan_song_count;
    return ESP_OK;
}
void music_library_reset(void) { count=0; }
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
{
    assert(ui_locked && !sd_locked);
    ++library_updates; last_library_count=songs;
    snprintf(last_library_status,sizeof(last_library_status),"%s",status ? status : "");
}
void ui_music_player_set_page(const music_page_t *page) { (void)page; }
void ui_music_player_bind(const ui_music_backend_t *backend) { (void)backend; }
/* UI policy and actual retained LVGL objects are covered by player_ui_tests;
 * this scheduling double asserts dark EOS is delegated only under UI lock. */
void ui_music_player_service_finished(bool animate)
{
    assert(ui_locked && !sd_locked && dark_screen && !animate);
    ++dark_eos_calls;
    if(!eos_ready) return;
    assert(music_player_adapter_is_playing()); /* BUSY covers EOS/request handoff. */
    eos_ready=false;++dark_eos_taken;
    ui_index=repeat_current?ui_index:(ui_index+1)%count;
    select_song(ui_index);
}
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
/* Deterministic scheduling harness: no sleeping, threads, or actual SD writes.
 * Keep the worker's real retry state and generation guards under test. */
static void prepare_scan_test(uint32_t songs)
{
    reset();
    ++generation;
    mounted=true;
    test_set_inserted(true);
    s_worker=(void *)1;
    s_seen_generation=UINT32_MAX;
    s_scan_state=MUSIC_SCAN_PENDING;
    s_scan_attempts=0;
    s_scan_retry_delay=0;
    s_scan_retry_start=0;
    test_ticks=0;
    lock_failures=scan_failures=scan_calls=notify_calls=library_updates=0;
    safe_remount_requests=0;
    scan_song_count=songs; count=0; ui_index=0;
    replace_card_in_scan=false;
    boot_anim_starts=0;
    dark_screen=eos_ready=repeat_current=false;dark_eos_calls=dark_eos_taken=0;
    saved_folder[0]=saved_audio[0]=0;
    atomic_store(&s_scan_requested,false);
    atomic_store(&s_requested_index,UINT32_MAX);
    last_library_status[0]=0;
}

static void run_restore_tests(void)
{
    prepare_scan_test(4);
    strcpy(saved_folder,"song2"); strcpy(saved_audio,"a2");
    media_worker_step();
    assert(ui_index==2 && atomic_load(&s_requested_index)==2 && !last_autoplay);
    media_worker_step();
    assert(shown==1 && s_selected_index==2 && !strcmp(saved_folder,"song2"));
    lvgl_port_lock(0); ui_index=3; select_song(3); lvgl_port_unlock();
    assert(last_autoplay);
    assert(music_player_adapter_pause(true));
    assert(music_player_adapter_capture_selection()==ESP_OK);
    assert(!strcmp(saved_folder,"song3") && !strcmp(saved_audio,"a3"));
    assert(music_player_adapter_pause(false));
    atomic_store(&s_worker_state,MEDIA_BUSY);
    assert(!music_player_adapter_pause(true));
    atomic_fetch_and(&s_worker_state,(unsigned)~MEDIA_BUSY);
    assert(music_player_adapter_pause(true));
    unsigned gate=0;
    assert(!atomic_compare_exchange_strong(&s_worker_state,&gate,MEDIA_BUSY));
    assert(music_player_adapter_pause(false));
    prepare_scan_test(4);
    strcpy(saved_folder,"song2"); strcpy(saved_audio,"missing.flac");
    media_worker_step(); assert(ui_index==0 && atomic_load(&s_requested_index)==0);
    puts("PASS stable folder/audio restore updates real UI selection, missing song falls back and pending click is captured on shutdown");
}

static void run_scan_tests(void)
{
    prepare_scan_test(4);
    lock_failures=1; /* Boot animation owns the SD mutex for the first attempt. */
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_WAIT_RETRY && s_scan_attempts==1 && scan_calls==0);
    assert(!ui_locked && !sd_locked);
    unsigned updates=library_updates;
    media_worker_step(); /* Polling must not busy-retry before the backoff expires. */
    assert(s_scan_attempts==1 && scan_calls==0 && library_updates==updates);
    music_player_adapter_request_scan_if_needed();
    media_worker_step(); /* Repeated unlock while waiting does not reset the retry budget. */
    assert(s_scan_attempts==1 && scan_calls==0);
    test_ticks=s_scan_retry_start+s_scan_retry_delay;
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==1 && last_library_count==4);
    assert(atomic_load(&s_requested_index)==0);
    assert(notify_calls>=1);
    for (unsigned i=0;i<4;i++) music_player_adapter_request_scan_if_needed();
    updates=library_updates;
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==1 && library_updates==updates);
    puts("PASS first boot SD lock timeout recovers without reinsertion; unlock preserves a ready library");

    prepare_scan_test(0);
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==1 && last_library_count==0);
    assert(boot_anim_starts==0); /* Flash resource startup owns animation, not SD scans. */
    updates=library_updates;
    for (unsigned i=0;i<4;i++) {
        music_player_adapter_request_scan_if_needed(); media_worker_step();
    }
    assert(scan_calls==1 && library_updates==updates);
    assert(atomic_load(&s_requested_index)==UINT32_MAX);
    puts("PASS successful empty catalogue is ready, not an endlessly retried failure");

    prepare_scan_test(4);
    scan_failures=2;
    for (unsigned i=0;i<3;i++) {
        media_worker_step();
        if (i<2) {
            assert(s_scan_state==MUSIC_SCAN_WAIT_RETRY && scan_calls==i+1);
            test_ticks=s_scan_retry_start+s_scan_retry_delay;
        }
    }
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==3 && s_scan_attempts==3);
    assert(safe_remount_requests==1);
    puts("PASS transient filesystem scan failures recover through bounded backoff");

    prepare_scan_test(4);
    lock_failures=10;
    for (unsigned i=0;i<MUSIC_SCAN_MAX_ATTEMPTS;i++) {
        media_worker_step(); test_ticks=s_scan_retry_start+s_scan_retry_delay;
    }
    assert(s_scan_state==MUSIC_SCAN_FAILED && scan_calls==0);
    unsigned failures_left=lock_failures;
    updates=library_updates;
    for (unsigned i=0;i<10;i++) { test_ticks+=10000; media_worker_step(); }
    assert(lock_failures==failures_left && library_updates==updates);
    lock_failures=0;
    music_player_adapter_request_scan_if_needed();
    music_player_adapter_request_scan_if_needed();
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==1 && s_scan_attempts==1);
    puts("PASS repeated failures stop; a later unlock merges requests and retries without remounting");

    prepare_scan_test(4);
    replace_card_in_scan=true;
    media_worker_step();
    assert(s_scan_state!=MUSIC_SCAN_READY && last_library_count==0);
    assert(atomic_load(&s_requested_index)==UINT32_MAX);
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==2 && s_seen_generation==generation);
    prepare_scan_test(4);
    lock_failures=1; media_worker_step();
    mounted=false; test_set_inserted(false); media_worker_step();
    assert(s_seen_generation==UINT32_MAX && last_library_count==0);
    mounted=true; test_set_inserted(true); ++generation;
    media_worker_step();
    assert(s_scan_state==MUSIC_SCAN_READY && scan_calls==1 && last_library_count==4);
    puts("PASS generation changes reject old scan results; removal cancels retry and reinsertion scans anew");
    reset();
}

static void run_dark_eos_tests(void)
{
    prepare_scan_test(4);media_worker_step();media_worker_step();
    dark_screen=true;eos_ready=true;
    atomic_store(&s_worker_state,MEDIA_BUSY);
    unsigned selected_before=audio_selects;
    media_worker_step();
    atomic_store(&s_worker_state,0);
    assert(dark_eos_taken==1 && audio_selects==selected_before+1 && ui_index==1);
    assert(last_autoplay && dark_screen && atomic_load(&s_requested_index)==UINT32_MAX);
    repeat_current=true;eos_ready=true;selected_before=audio_selects;
    atomic_store(&s_worker_state,MEDIA_BUSY);media_worker_step();atomic_store(&s_worker_state,0);
    assert(dark_eos_taken==2 && audio_selects==selected_before+1 && ui_index==1 && last_autoplay);
    eos_ready=true;dark_screen=false;unsigned handled=dark_eos_calls;
    media_worker_step();assert(dark_eos_calls==handled && eos_ready);
    dark_screen=true;
    assert(music_player_adapter_pause(true));
    media_worker_step();assert(dark_eos_calls==handled && eos_ready);
    assert(music_player_adapter_pause(false));
    eos_ready=false;media_worker_step(); /* Pause/error without natural EOS never selects. */
    selected_before=audio_selects;media_worker_step();assert(audio_selects==selected_before);
    mounted=false;test_set_inserted(false);eos_ready=true;handled=dark_eos_calls;
    media_worker_step();assert(dark_eos_calls==handled && !eos_ready && !last_autoplay);
    dark_screen=false;reset();
    puts("PASS dark media-worker EOS handoff, current-repeat, awake UI ownership, pause and card-removal gates");
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
    run_scan_tests();
    run_restore_tests();
    run_dark_eos_tests();
    puts("ALL ADAPTER TESTS PASSED");
    return 0;
}
