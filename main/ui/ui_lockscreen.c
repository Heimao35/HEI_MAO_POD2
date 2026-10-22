#include "ui_lockscreen.h"
#include "ui_statusbar.h"
#include "ui_fonts.h"
#include "lock_animation.h"
#include "music_player_adapter.h"
#include "video_player.h"
#include "jpeg_codec.h"
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
/* 右下角姿态传感器读数由 BMI270 后台发布，接口只更新 RAM/标签；
 * 未发布前显示“--”，不能用 0 或演示值冒充真实测量。 */
static lv_obj_t *s_steps_label = NULL;
static lv_obj_t *s_temp_label = NULL;
static bool s_steps_valid, s_temp_valid;
static uint32_t s_steps_value;
static int16_t s_temp_x10;
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

    /* 视频播放器可能正在停止；先等它空闲再开始动画，避免 UI 帧缓冲交接
     * 与锁屏重播重叠。JPEG 引擎本身由 jpeg_codec 全局共享并串行化。 */
    for (unsigned i=0; i<100 && !video_player_is_idle(); ++i)
        vTaskDelay(pdMS_TO_TICKS(5));

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    TickType_t frame_ticks = pdMS_TO_TICKS(1000 / ANIM_FPS);
    while (atomic_load(&s_visible) && generation == atomic_load(&s_anim_generation)) {
        TickType_t t0 = xTaskGetTickCount();
        uint32_t out_size = 0;
        esp_err_t ret = jpeg_codec_process(&dec_cfg,
                                            frame, (uint32_t)frame_len,
                                            s_decode_buf, DECODE_BUF_SZ,
                                            &out_size, 1000);
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

/**
 * @brief 刷新右下角步数/温度文本。
 *
 * 两项都按“有有效数据才显示数值”的原则处理：没有传感器数据时显示
 * “-- 步”/“--℃”，而不是显示 0 或占位温度。温度用整数十分之一度
 * 格式化，避免在 UI 层引入浮点打印。调用方需持有 LVGL 锁（定时器或
 * 传感器发布路径均在锁内）。
 */
static void sensor_labels_update(void)
{
    if (!s_steps_label || !s_temp_label) return;
    char buf[32];
    if (s_steps_valid) {
        snprintf(buf, sizeof(buf), "%lu 步", (unsigned long)s_steps_value);
    } else {
        snprintf(buf, sizeof(buf), "-- 步");
    }
    lv_label_set_text(s_steps_label, buf);

    if (s_temp_valid) {
        int32_t t = s_temp_x10;
        const char *sign = (t < 0) ? "-" : "";
        uint32_t a = (uint32_t)(t < 0 ? -t : t);
        snprintf(buf, sizeof(buf), "%s%lu.%lu℃", sign,
                 (unsigned long)(a / 10U), (unsigned long)(a % 10U));
    } else {
        snprintf(buf, sizeof(buf), "--℃");
    }
    lv_label_set_text(s_temp_label, buf);
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

    /* 右下角传感器读数：温度在步数上方，两者右边缘对齐。
     * 使用固定宽度 + TEXT_ALIGN_RIGHT，而不是 align_to 依赖兄弟标签宽度：
     * 步数位数会变化，align_to 只在创建时计算一次，跨过 9→10、99→100
     * 后右边缘会漂移。固定宽度盒子锚定在 BOTTOM_RIGHT，文本在盒内右对齐，
     * 无论 1 位还是 5 位数都紧贴同一右边距。
     * 字号 22（用户允许的最大字号），与锁屏星期行一致；白色/次级灰沿用
     * 现有锁屏配色，不引入新颜色或遮罩。标签不可点击，不阻挡上滑解锁。 */
    s_steps_label = lv_label_create(s_lock_page);
    lv_obj_set_width(s_steps_label, 200);
    lv_obj_set_style_text_align(s_steps_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_steps_label, "-- 步");
    lv_obj_set_style_text_color(s_steps_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_steps_label, ui_font_22(), 0);
    lv_obj_align(s_steps_label, LV_ALIGN_BOTTOM_RIGHT, -20, -14);

    s_temp_label = lv_label_create(s_lock_page);
    lv_obj_set_width(s_temp_label, 200);
    lv_obj_set_style_text_align(s_temp_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_temp_label, "--℃");
    lv_obj_set_style_text_color(s_temp_label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_temp_label, ui_font_22(), 0);
    /* 步数标签高约 26px、底边距 14px；温度底边放在 -52px，留出约 12px 间距。 */
    lv_obj_align(s_temp_label, LV_ALIGN_BOTTOM_RIGHT, -20, -52);

    s_steps_valid = false;
    s_temp_valid = false;
    s_steps_value = 0;
    s_temp_x10 = 0;

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

    /* 每日零点清零现由BMI270后台根据真实RTC日期统一处理，熄屏时也
     * 会推进；UI只显示带有效性标记的数据，不再独立重置另一份计数，
     * 避免RTC发布/传感器发布先后顺序不同而把刚收到的新一天步数清掉。 */
    sensor_labels_update();
}

void ui_lockscreen_set_steps(bool valid, uint32_t steps)
{
    /* 发布方持有LVGL锁；参数已由后台换算为当天步数。此处只更新
     * 原有标签，不访问RTC/I2C，也不因后台采样而触发活动时间或亮屏。 */
    s_steps_valid = valid;
    if (valid) s_steps_value = steps;
    sensor_labels_update();
}

void ui_lockscreen_set_temperature(bool valid, int16_t temp_c_x10)
{
    /* 与步数一样只更新显示；温度有效性由BMI270 后台发布方
     * 根据芯片状态判断；这里显示芯片温度，不推测环境温度。 */
    s_temp_valid = valid;
    if (valid) s_temp_x10 = temp_c_x10;
    sensor_labels_update();
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
