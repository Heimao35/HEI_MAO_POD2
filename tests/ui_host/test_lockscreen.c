/* Real lock UI, animation cancellation/replay and JPEG staging. Hardware and
 * task doubles avoid a second real thread; the production worker ownership
 * and generation checks are inspected through deterministic decode hooks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Clock layout is not under test; avoid adding a 48px host font dependency. */
#define lv_font_montserrat_48 lv_font_montserrat_26
#include "../../main/ui/ui_lockscreen.c"

static bool ui_locked, sd_locked, mounted = true, present = true, asset_available=true;
static unsigned sd_accesses;
static unsigned task_creations, notifications, scans, decodes, engines, deletes;
static bool cancel_during_decode, fail_engine;
/* 模拟“JPEG 引擎尚在创建”或“引擎被其他模块占用”的瞬态错误；锁屏必须
 * 保留当前帧重试，而不是把帧读掉后从中间开始播放。 */
static unsigned transient_failures;
static bool show_status_time;
static TickType_t ticks;
static lv_obj_t *bar;
static unsigned cover_events;
static bool covered_state;
static void visibility_cb(bool covered) { ++cover_events; covered_state = covered; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }
static ui_device_state_t state = {.time_valid=true, .weekday=0, .hour=16, .minute=45, .year=2026, .month=10, .day=18};

bool lvgl_port_lock(uint32_t timeout) { (void)timeout; assert(!ui_locked && !sd_locked); ui_locked=true; return true; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked=false; }
ui_device_state_t *ui_get_device_state(void) { return &state; }
void ui_statusbar_show_time(bool value) { show_status_time=value; }
lv_obj_t *ui_statusbar_get_obj(void) { return bar; }
void music_player_adapter_request_scan_if_needed(void) { ++scans; }
bool video_player_is_idle(void) { return true; }
bool sd_card_io_lock(TickType_t wait) { (void)wait; ++sd_accesses; assert(!"lock replay must never access SD"); return false; }
void sd_card_io_unlock(void) { assert(sd_locked); sd_locked=false; }
bool sd_card_is_mounted(void) { return mounted; }
bool sd_card_is_present(void) { return present; }
/* Asset service is tested separately with real Flash import/reboot faults.
 * This test retains the actual UI/JPEG worker and checks cache-only replay. */
bool lock_animation_next_frame(size_t *position,const uint8_t **frame,size_t *bytes)
{
    static const uint8_t data[]={0xFF,0xD8,0xFF,0xD9};
    assert(!ui_locked && !sd_locked);
    if (!asset_available || *position) return false;
    *frame=data; *bytes=sizeof(data); *position=sizeof(data); return true;
}
TickType_t xTaskGetTickCount(void) { return ticks++; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)1); ++notifications; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t delay) { (void)clear; (void)delay; return 0; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                       UBaseType_t priority, TaskHandle_t *task)
{ assert(fn==mjpeg_anim_task && !strcmp(name,"lock_anim") && stack==8192 && !arg && priority==5); ++task_creations; *task=(void *)1; return pdPASS; }
void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *actual)
{ assert(cfg->buffer_direction == JPEG_DEC_ALLOC_OUTPUT_BUFFER); *actual=size; return calloc(1,size); }
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *decoder)
{
    assert(!sd_locked && cfg->timeout_ms==100);
    if (fail_engine) { *decoder=NULL; return ESP_FAIL; }
    ++engines; *decoder=(void *)2; return ESP_OK;
}
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t decoder)
{ assert(decoder==(void *)2); ++deletes; return ESP_OK; }
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t decoder, const jpeg_decode_cfg_t *cfg,
                              const uint8_t *input, uint32_t bytes, uint8_t *output,
                              uint32_t capacity, uint32_t *size)
{
    assert(!ui_locked && !sd_locked && decoder==(void *)2);
    assert(cfg->output_format==JPEG_DECODE_OUT_FORMAT_RGB565 && bytes==4 && input[0]==0xFF);
    /* 解码目标必须是"当前不在显示"的那一块；canvas 指向另一块。 */
    assert(output==s_frame_bufs[s_frame_index] && capacity==DECODE_BUF_SZ);
    assert(output!=lv_canvas_get_buf(s_bg_canvas));
    ++decodes; memset(output,0x5A,capacity); *size=capacity;
    if (cancel_during_decode) ui_lockscreen_stop_anim();
    return ESP_OK;
}
esp_err_t jpeg_codec_process(const jpeg_decode_cfg_t *cfg,
                             const uint8_t *input, uint32_t input_len,
                             uint8_t *output, uint32_t output_cap,
                             uint32_t *output_len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (fail_engine) return ESP_FAIL;
    if (transient_failures) {
        --transient_failures;
        return ESP_ERR_INVALID_STATE;
    }
    return jpeg_decoder_process((jpeg_decoder_handle_t)(void *)2, cfg,
                                input, input_len, output, output_cap, output_len);
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(void)
{
    lv_timer_handler(); /* starts animations using the host's manual tick */
    lv_tick_inc(UI_ANIM_DURATION_MS+20); lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}
static void run_requested(void)
{
    atomic_store(&s_anim_busy,true);
    unsigned generation=atomic_exchange(&s_anim_requested,0);
    if (generation && generation==atomic_load(&s_anim_generation)) play_mjpeg(generation);
    atomic_store(&s_anim_busy,false);
}
/* Time strings and the custom weekday font have different glyph widths.
 * Every line needs a fixed page-width text box with centered text, not merely
 * a once-computed position relative to a content-sized sibling label. */
static void assert_clock_horizontally_centered(void)
{
    lv_obj_update_layout(s_lock_page);
    lv_area_t page;lv_obj_get_coords(s_lock_page,&page);
    lv_obj_t *labels[]={s_time_label,s_date_label,s_weekday_label};
    for(unsigned i=0;i<sizeof(labels)/sizeof(labels[0]);++i) {
        lv_area_t area;lv_obj_get_coords(labels[i],&area);
        assert(lv_obj_get_width(labels[i])==UI_HOR_RES);
        assert(area.x1+area.x2==page.x1+page.x2);
        assert(lv_obj_get_style_text_align(labels[i],LV_PART_MAIN)==LV_TEXT_ALIGN_CENTER);
    }
}

/* 三行之间的固定10px行距。 */
static void assert_clock_line_gaps(void)
{
    assert(lv_obj_get_y(s_date_label)==lv_obj_get_y(s_time_label)+lv_obj_get_height(s_time_label)+CLOCK_LINE_GAP);
    assert(lv_obj_get_y(s_weekday_label)==lv_obj_get_y(s_date_label)+lv_obj_get_height(s_date_label)+CLOCK_LINE_GAP);
}

/* 三行作为整体垂直居中；整数取整允许1px误差。 */
static void assert_clock_centered(void)
{
    assert_clock_horizontally_centered();
    lv_area_t page;lv_obj_get_coords(s_lock_page,&page);
    lv_area_t first,last;
    lv_obj_get_coords(s_time_label,&first);
    lv_obj_get_coords(s_weekday_label,&last);
    int32_t block_center=(first.y1+last.y2)/2;
    int32_t page_center=(page.y1+page.y2)/2;
    assert(block_center>=page_center-1 && block_center<=page_center+1);
    assert_clock_line_gaps();
}
int main(void)
{
    lv_init(); lv_display_t *display=lv_display_create(460,460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    ui_lockscreen_create(lv_screen_active());
    /* 画布直接用 464x464 解码缓冲，靠 460x460 的页面裁掉右/下各 4 像素，
     * 不再每帧把 464 宽逐行拷成 460 宽。 */
    lv_obj_update_layout(s_lock_page);
    assert(s_frame_bufs[0] && s_frame_bufs[1] && s_frame_bufs[0]!=s_frame_bufs[1]);
    assert(lv_obj_get_width(s_bg_canvas)==(int32_t)PADDED_W);
    assert(lv_obj_get_height(s_bg_canvas)==(int32_t)PADDED_H);
    assert(lv_obj_get_x(s_bg_canvas)==0 && lv_obj_get_y(s_bg_canvas)==0);
    assert(PADDED_W>FRAME_W && PADDED_H>FRAME_H);
    assert(lv_canvas_get_buf(s_bg_canvas)==s_frame_bufs[0]);
    bar=lv_obj_create(lv_screen_active());
    assert(task_creations==1 && ui_lockscreen_is_visible());
    ui_lockscreen_set_visibility_cb(visibility_cb);
    assert(cover_events==0);
    ui_lockscreen_update();
    assert(!strcmp(lv_label_get_text(s_time_label),"16:45"));
    assert(!strcmp(lv_label_get_text(s_date_label),"2026-10-18"));
    assert(!strcmp(lv_label_get_text(s_weekday_label),"星期天"));
    assert_clock_centered();
    state.time_valid=false;ui_lockscreen_update();
    assert(!strcmp(lv_label_get_text(s_time_label),"--:--"));
    assert(!strcmp(lv_label_get_text(s_weekday_label),"时间未设置"));
    assert_clock_centered();
    state.time_valid=true;
    state.hour=1;state.minute=11;state.month=1;state.day=1;state.weekday=1;
    ui_lockscreen_update();assert_clock_centered();
    state.hour=23;state.minute=58;state.month=12;state.day=31;state.weekday=6;
    ui_lockscreen_update();assert_clock_centered();
    /* Changing weekday line metrics is a test-only check: the production
     * layout is centered once for the fixed 22px weekday font. Verify that
     * horizontal centering and the original 10px gap do not depend on glyph
     * widths, then restore the font and verify the whole block centering. */
    lv_obj_set_style_text_font(s_weekday_label,&lv_font_montserrat_16,0);
    ui_lockscreen_update();
    assert_clock_horizontally_centered();
    assert_clock_line_gaps();
    lv_obj_set_style_text_font(s_weekday_label,ui_font_22(),0);
    ui_lockscreen_update();assert_clock_centered();

    ui_lockscreen_start_boot_anim(); unsigned first=atomic_load(&s_anim_requested);
    assert(first && !ui_lockscreen_anim_is_idle());
    ui_lockscreen_show(); unsigned second=atomic_load(&s_anim_requested);
    assert(second>first && task_creations==1 && !show_status_time);
    assert(covered_state && cover_events==1);
    /* Initial lock is already visible: replay changes only the MJPEG request,
     * never schedules a second top-down page entry animation. */
    assert(lv_anim_get(s_lock_page,anim_y_cb)==NULL);
    lv_obj_update_layout(s_lock_page);
    assert(lv_obj_get_y(s_lock_page)==0);
    assert(lv_obj_get_index(bar)>lv_obj_get_index(s_lock_page));
    puts("PASS already-visible lock replays MJPEG in place with no page slide and preserves status-bar layer");

    ui_lockscreen_hide();
    assert(!covered_state);
    ui_lockscreen_show();
    assert(covered_state);
    advance();
    assert(ui_lockscreen_is_visible() && !lv_obj_has_flag(s_lock_page,LV_OBJ_FLAG_HIDDEN));
    assert(lv_anim_get(s_lock_page,anim_y_cb)==NULL);
    ui_lockscreen_hide();
    assert(!covered_state);
    advance();
    assert(!ui_lockscreen_is_visible() && show_status_time && scans==1);
    assert(ui_lockscreen_anim_is_idle());
    unsigned notify_before=notifications; ui_lockscreen_start_boot_anim();
    assert(notifications==notify_before);
    puts("PASS rapid hide/replay cancels stale completion; normal unlock retains scan request");

    /* A genuine entry from the unlocked page keeps the original duration and
     * ease-out trajectory. Replaying after that entry must not slide again. */
    ui_lockscreen_show();
    lv_anim_t *slide=lv_anim_get(s_lock_page,anim_y_cb);
    assert(slide && slide->start_value==-(int)UI_VER_RES && slide->end_value==0);
    assert(slide->path_cb==UI_ANIM_PATH);
    /* 模式页在滑入完成前仍可见，滑入完成后才收到 covered=true。 */
    assert(!covered_state);
    advance(); assert(lv_obj_get_y(s_lock_page)==0);
    assert(covered_state);
    ui_lockscreen_stop_anim(); /* screen off retains visibility and objects */
    unsigned wake_generation=atomic_load(&s_anim_generation);
    ui_lockscreen_show();
    assert(atomic_load(&s_anim_requested)>wake_generation);
    assert(covered_state);
    assert(lv_anim_get(s_lock_page,anim_y_cb)==NULL);
    lv_obj_update_layout(s_lock_page); assert(lv_obj_get_y(s_lock_page)==0);
    assert(scans==1 && task_creations==1);
    puts("PASS unlocked entry keeps ease-out slide; wake replays only MJPEG without recreating UI or rescanning SD");

    asset_available=false; ui_lockscreen_show(); run_requested();
    assert(ui_lockscreen_anim_is_idle() && !engines && !sd_accesses);
    asset_available=true;
    /* SDK partial cleanup must return an error to this existing application
     * branch. Keep the SD lock/worker idle and allow the next entry to retry. */
    fail_engine=true; ui_lockscreen_show(); run_requested();
    assert(!sd_locked && !ui_locked && !engines && !deletes && !decodes);
    assert(ui_lockscreen_anim_is_idle());
    fail_engine=false;
    puts("PASS decoder allocation failure exits safely and allows the next lock replay");
    ui_lockscreen_show(); run_requested();
    /* 解码输出被直接发布为 canvas 缓冲：内容来自刚解码的那一块，不是拷贝。 */
    uint8_t *shown = s_frame_bufs[s_frame_index ^ 1U];
    assert(decodes==1 && shown[0]==0x5A && shown[DECODE_BUF_SZ-1]==0x5A);
    assert(lv_canvas_get_buf(s_bg_canvas)==shown);
    assert(engines==deletes && ui_lockscreen_anim_is_idle());
    ui_lockscreen_show(); assert(s_frame_bufs[s_frame_index ^ 1U][0]==0);
    cancel_during_decode=true; run_requested();
    /* 取消的解码不能发布，也不能切换缓冲：显示中的一块仍是清空后的状态。 */
    assert(decodes==2 && s_frame_bufs[s_frame_index ^ 1U][0]==0 && engines==deletes);
    notify_before=notifications; ui_lockscreen_start_boot_anim(); assert(notifications==notify_before);
    puts("PASS each entry resets old tail frame; canceled decoder cannot publish; dark state rejects late SD replay");

    ui_lockscreen_show(); cancel_during_decode=false; mounted=false; run_requested();
    assert(!sd_locked && decodes==3 && ui_lockscreen_anim_is_idle());
    mounted=true; present=false; ui_lockscreen_show(); run_requested();
    assert(!sd_locked && decodes==4 && engines==deletes && !sd_accesses);
    asset_available=false; ui_lockscreen_show(); run_requested();
    assert(decodes==4 && ui_lockscreen_anim_is_idle() && engines==deletes && task_creations==1);
    puts("PASS Flash cache replays without mounted/present SD, no SD lock, no worker or engine accumulation");

    /* 引擎尚未就绪/被占用：前两次返回 INVALID_STATE，第三次成功。锁屏
     * 必须重试同一帧，等引擎可用后仍显示第一帧，而不是把帧消耗掉。 */
    asset_available = true;
    transient_failures = 2;
    ui_lockscreen_show();
    run_requested();
    assert(decodes == 5 && s_frame_bufs[s_frame_index ^ 1U][0] == 0x5A);
    assert(ui_lockscreen_anim_is_idle() && engines == deletes);
    puts("PASS transient decoder unavailability preserves the frame and then decodes it");

    lv_deinit(); free(s_frame_bufs[0]); free(s_frame_bufs[1]);
    puts("ALL LOCKSCREEN TESTS PASSED"); return 0;
}
