#include "ui_lockscreen.h"
#include "ui_statusbar.h"
#include "ui_fonts.h"
#include "lock_animation.h"
#include "music_player_adapter.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "lockscr";

#define FRAME_W    LOCK_ANIMATION_WIDTH
#define FRAME_H    LOCK_ANIMATION_HEIGHT
#define FRAME_BPP  2
#define FRAME_BUF_SZ (FRAME_W * FRAME_H * FRAME_BPP)
#define PAD16(x)   (((x) + 15) & ~15)
#define PADDED_W   PAD16(FRAME_W)
#define PADDED_H   PAD16(FRAME_H)
#define DECODE_BUF_SZ (PADDED_W * PADDED_H * FRAME_BPP)
#define ANIM_FPS   LOCK_ANIMATION_FPS

static lv_obj_t *s_lock_page  = NULL;
static lv_obj_t *s_bg_canvas  = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_weekday_label = NULL;
static atomic_bool s_visible = true;
/* 单一常驻任务独占 JPEG 解码器和缓冲区。每次重播递增代次，旧任务
 * 即使仍在解码，也不能再发布旧帧；避免快速锁屏/解锁重叠写图。 */
static atomic_bool s_anim_busy;
static atomic_bool s_anim_allowed = true;
static atomic_uint s_anim_requested;
static atomic_uint s_anim_generation;
static TaskHandle_t s_anim_task;
static portMUX_TYPE s_anim_request_lock = portMUX_INITIALIZER_UNLOCKED;


static uint8_t *s_rgb_buf = NULL;
static uint8_t *s_decode_buf = NULL;

static void play_mjpeg(unsigned generation)
{
    /* 启动阶段已完成SD导入、Flash校验和只读PSRAM缓存发布；重播只取缓存。
     * 不能在这里检查SD文件/更新时间：即使只是一次大文件读取，也会阻塞
     * 正在补充音频数据的同一个SD锁。无缓存时保留时间/日期和原有手势。
     * 每次调用的position从0开始；缓存直到关机都不变，无需新分配文件缓冲。 */
    size_t position=0, frame_len=0;
    const uint8_t *frame=NULL;
    if (!lock_animation_next_frame(&position,&frame,&frame_len)) return;

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    if (jpeg_new_decoder_engine(&eng_cfg, &decoder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder");
        return;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    TickType_t frame_ticks = pdMS_TO_TICKS(1000 / ANIM_FPS);
    while (atomic_load(&s_visible) && generation == atomic_load(&s_anim_generation)) {
        TickType_t t0 = xTaskGetTickCount();
        uint32_t out_size = 0;
        esp_err_t ret = jpeg_decoder_process(decoder, &dec_cfg,
                                              frame, (uint32_t)frame_len,
                                              s_decode_buf, DECODE_BUF_SZ,
                                              &out_size);
        if (ret == ESP_OK) {
            if (lvgl_port_lock(50)) {
                /* 先确认本轮仍有效，再在 LVGL 锁内更新可见 canvas。
                 * 解码始终使用独立的 staging buffer，绘制不会读到半帧。 */
                if (atomic_load(&s_visible) && generation == atomic_load(&s_anim_generation)) {
                    for (int row = 0; row < FRAME_H; row++) {
                        memcpy(s_rgb_buf + row * FRAME_W * FRAME_BPP,
                               s_decode_buf + row * PADDED_W * FRAME_BPP,
                               FRAME_W * FRAME_BPP);
                    }
                    lv_canvas_set_buffer(s_bg_canvas, s_rgb_buf, FRAME_W, FRAME_H,
                                         LV_COLOR_FORMAT_RGB565);
                    lv_obj_invalidate(s_bg_canvas);
                }
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGW(TAG, "JPEG decode err: %s", esp_err_to_name(ret));
        }

        TickType_t elapsed = xTaskGetTickCount() - t0;
        if (elapsed < frame_ticks) {
            vTaskDelay(frame_ticks - elapsed);
        }
        if (!lock_animation_next_frame(&position,&frame,&frame_len)) break;
    }

    jpeg_del_decoder_engine(decoder);
    ESP_LOGI(TAG, "Lock animation finished");
}

static void mjpeg_anim_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_anim_request_lock);
        atomic_store(&s_anim_busy, true);
        unsigned generation = atomic_exchange(&s_anim_requested, 0);
        portEXIT_CRITICAL(&s_anim_request_lock);
        if (generation && generation == atomic_load(&s_anim_generation)) play_mjpeg(generation);
        atomic_store(&s_anim_busy, false);
    }
}

void ui_lockscreen_create(lv_obj_t *parent)
{
    s_lock_page = lv_obj_create(parent);
    lv_obj_set_size(s_lock_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_lock_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_lock_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_lock_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lock_page, 0, 0);
    lv_obj_set_style_radius(s_lock_page, 0, 0);
    lv_obj_set_style_pad_all(s_lock_page, 0, 0);
    lv_obj_remove_flag(s_lock_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* MJPEG 动画背景 canvas（全屏，最底层） */
    s_rgb_buf = heap_caps_aligned_calloc(128, 1, FRAME_BUF_SZ,
                                          MALLOC_CAP_SPIRAM);
    jpeg_decode_memory_alloc_cfg_t dec_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t dec_buf_actual = 0;
    s_decode_buf = jpeg_alloc_decoder_mem(DECODE_BUF_SZ, &dec_mem_cfg,
                                          &dec_buf_actual);
    s_bg_canvas = lv_canvas_create(s_lock_page);
    lv_obj_set_size(s_bg_canvas, FRAME_W, FRAME_H);
    lv_obj_align(s_bg_canvas, LV_ALIGN_CENTER, 0, 0);
    if (s_rgb_buf) {
        lv_canvas_set_buffer(s_bg_canvas, s_rgb_buf,
                             FRAME_W, FRAME_H, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_add_flag(s_bg_canvas, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 三行统一使用整屏宽度的文本框，并在框内居中。仅把内容自适应的
     * label 对象放到中间，不能保证文本居中；尤其 align_to 是一次性
     * 计算，RTC 占位文本切到真实时间/星期后宽度改变会让日期或星期偏移。
     * 固定宽度避免依赖兄弟文本的旧宽度，不改变原纵向位置、字号和间距。
     * 这里只设标签文本对齐，不改锁屏容器/触摸范围及 MJPEG 动画逻辑。 */
    s_time_label = lv_label_create(s_lock_page);
    lv_obj_set_width(s_time_label,UI_HOR_RES);
    lv_obj_set_style_text_align(s_time_label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, UI_VER_RES * 30 / 100);

    s_date_label = lv_label_create(s_lock_page);
    lv_obj_set_width(s_date_label,UI_HOR_RES);
    lv_obj_set_style_text_align(s_date_label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(s_date_label, "---- -- --");
    lv_obj_set_style_text_color(s_date_label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_26, 0);
    lv_obj_align_to(s_date_label, s_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* 新增第三行星期：只占用日期下方空间，保留原大时间、日期、背景
     * canvas 和解锁触摸区域的位置。中文采用 22 号，与数字字体分开。
     * 依次更新后重新相对对齐，避免日期/占位文本宽度变化造成星期偏移。 */
    s_weekday_label=lv_label_create(s_lock_page);
    lv_obj_set_width(s_weekday_label,UI_HOR_RES);
    lv_obj_set_style_text_align(s_weekday_label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(s_weekday_label,"时间未设置");
    lv_obj_set_style_text_font(s_weekday_label,ui_font_22(),0);
    lv_obj_set_style_text_color(s_weekday_label,UI_COLOR_TEXT_SECONDARY,0);
    lv_obj_align_to(s_weekday_label,s_date_label,LV_ALIGN_OUT_BOTTOM_MID,0,10);

    s_visible = true;
    if (s_rgb_buf && s_decode_buf &&
        xTaskCreate(mjpeg_anim_task, "lock_anim", 8192, NULL, 5, &s_anim_task) != pdPASS)
        ESP_LOGE(TAG, "Cannot create lock animation worker");
}

void ui_lockscreen_update(void)
{
    if (!s_lock_page || !s_visible) return;
    ui_device_state_t *st = ui_get_device_state();
    char buf[32];
    if(st->time_valid) snprintf(buf,sizeof(buf),"%02d:%02d",st->hour,st->minute);
    else snprintf(buf,sizeof(buf),"--:--");
    lv_label_set_text(s_time_label,buf);
    if(st->time_valid) snprintf(buf,sizeof(buf),"%04d-%02d-%02d",st->year,st->month,st->day);
    else snprintf(buf,sizeof(buf),"---- -- --");
    lv_label_set_text(s_date_label,buf);
    static const char *const weekdays[]={"天","一","二","三","四","五","六"};
    if(st->time_valid && st->weekday<=6) snprintf(buf,sizeof(buf),"星期%s",weekdays[st->weekday]);
    else snprintf(buf,sizeof(buf),"时间未设置");
    lv_label_set_text(s_weekday_label,buf);
    lv_obj_align_to(s_weekday_label,s_date_label,LV_ALIGN_OUT_BOTTOM_MID,0,10);
}

static void show_anim_ready(lv_anim_t *a)
{
    (void)a;
    s_visible = true;
    ui_statusbar_show_time(false);
}

static void hide_anim_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_lock_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ui_lockscreen_stop_anim();
    ui_statusbar_show_time(true);
    /* 解锁动画结束只投递补扫请求，不在 LVGL 回调内等待 SD。已成功扫描
     * 的曲库不会重建；开机首次失败才由后台执行有限重试。 */
    music_player_adapter_request_scan_if_needed();
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

void ui_lockscreen_show(void)
{
    if (!s_lock_page) return;
    /* 页面位移与 MJPEG 重播是两件独立的事：只有从已解锁页面进入锁屏
     * 才从顶部滑入。熄屏时锁屏对象仍然可见，唤醒只复位到 y=0 并重播
     * MJPEG，不能把已有锁屏再次从屏幕外滑进来。
     * 先取消旧位移动画和解码代次，防止旧 hide 回调或旧 JPEG 帧覆盖
     * 本次唤醒；这里由调用方持有 LVGL 锁，清空 canvas 不与绘制并发。 */
    bool slide_in = !atomic_load(&s_visible);
    ui_lockscreen_stop_anim();
    lv_anim_delete(s_lock_page, anim_y_cb);
    lv_obj_set_y(s_lock_page, slide_in ? -(int32_t)UI_VER_RES : 0);
    lv_obj_remove_flag(s_lock_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_page);
    lv_obj_t *bar = ui_statusbar_get_obj();
    if (bar) lv_obj_move_foreground(bar);

    if (slide_in) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lock_page);
        lv_anim_set_values(&a, -(int32_t)UI_VER_RES, 0);
        lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
        lv_anim_set_path_cb(&a, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_completed_cb(&a, show_anim_ready);
        lv_anim_start(&a);
    }

    s_visible = true;
    atomic_store(&s_anim_allowed, true);
    /* 清空上一轮尾帧并立即更新时间；新请求从 MJPEG 的首帧重新解码。
     * 常驻worker只读取启动时发布的动画缓存，不在触摸/显示路径访问SD或Flash。 */
    if (s_rgb_buf) memset(s_rgb_buf, 0, FRAME_BUF_SZ);
    lv_obj_invalidate(s_bg_canvas);
    ui_lockscreen_update();
    ui_statusbar_show_time(false);
    ui_lockscreen_start_boot_anim();
}

void ui_lockscreen_hide(void)
{
    if (!s_lock_page || !s_visible) return;
    ui_lockscreen_stop_anim();
    lv_anim_delete(s_lock_page, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock_page);
    lv_anim_set_values(&a, 0, -(int32_t)UI_VER_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, hide_anim_ready);
    lv_anim_start(&a);
}

bool ui_lockscreen_is_visible(void)
{
    return s_visible;
}

void ui_lockscreen_start_boot_anim(void)
{
    /* 启动资源就绪或页面重播时只投递标量请求，不访问LVGL对象或文件。 */
    portENTER_CRITICAL(&s_anim_request_lock);
    bool allowed = s_anim_task && atomic_load(&s_visible) && atomic_load(&s_anim_allowed);
    if (allowed) {
        unsigned generation = atomic_fetch_add(&s_anim_generation, 1) + 1;
        atomic_store(&s_anim_requested, generation);
    }
    portEXIT_CRITICAL(&s_anim_request_lock);
    if (allowed) xTaskNotifyGive(s_anim_task);
}

void ui_lockscreen_stop_anim(void)
{
    /* 熄屏/解锁后屏蔽迟到的开机动画请求；再次show才开放。 */
    portENTER_CRITICAL(&s_anim_request_lock);
    atomic_store(&s_anim_allowed, false);
    atomic_store(&s_anim_requested, 0);
    atomic_fetch_add(&s_anim_generation, 1);
    portEXIT_CRITICAL(&s_anim_request_lock);
}

bool ui_lockscreen_anim_is_idle(void)
{
    portENTER_CRITICAL(&s_anim_request_lock);
    bool idle = !atomic_load(&s_anim_busy) && !atomic_load(&s_anim_requested);
    portEXIT_CRITICAL(&s_anim_request_lock);
    return idle;
}
