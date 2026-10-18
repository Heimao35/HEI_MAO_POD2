#include "ui_lockscreen.h"
#include "ui_statusbar.h"
#include "sd_card.h"
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

#define MJPEG_PATH SD_SYS_DIR "/MEDIA/lock_scr/000.mjpeg"
#define FRAME_W    460
#define FRAME_H    460
#define FRAME_BPP  2
#define FRAME_BUF_SZ (FRAME_W * FRAME_H * FRAME_BPP)
#define PAD16(x)   (((x) + 15) & ~15)
#define PADDED_W   PAD16(FRAME_W)
#define PADDED_H   PAD16(FRAME_H)
#define DECODE_BUF_SZ (PADDED_W * PADDED_H * FRAME_BPP)
#define ANIM_FPS   24

static lv_obj_t *s_lock_page  = NULL;
static lv_obj_t *s_bg_canvas  = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static atomic_bool s_visible = true;
/* 单一常驻任务独占 JPEG 解码器和缓冲区。每次重播递增代次，旧任务
 * 即使仍在读 SD/解码，也不能再发布旧帧；避免快速锁屏/解锁重叠写图。 */
static atomic_bool s_anim_busy;
static atomic_bool s_anim_allowed = true;
static atomic_uint s_anim_requested;
static atomic_uint s_anim_generation;
static TaskHandle_t s_anim_task;
static portMUX_TYPE s_anim_request_lock = portMUX_INITIALIZER_UNLOCKED;


static uint8_t *s_rgb_buf = NULL;
static uint8_t *s_decode_buf = NULL;

static const uint8_t SOI_MARKER[] = {0xFF, 0xD8};
static const uint8_t EOI_MARKER[] = {0xFF, 0xD9};

static int find_marker(const uint8_t *buf, int len, const uint8_t *marker)
{
    for (int i = 0; i < len - 1; i++) {
        if (buf[i] == marker[0] && buf[i + 1] == marker[1]) return i;
    }
    return -1;
}

static void play_mjpeg(unsigned generation)
{

    /* Share SD handle lifetime protection with the music worker. */
    if (!sd_card_io_lock(portMAX_DELAY)) {
        return;
    }
    if (!sd_card_is_mounted() || !sd_card_is_present()) {
        sd_card_io_unlock();
        return;
    }
    FILE *fp = fopen(MJPEG_PATH, "rb");
    if (!fp) {
        sd_card_io_unlock();
        ESP_LOGW(TAG, "No boot animation file: %s", MJPEG_PATH);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "MJPEG file size invalid: %ld", file_size);
        fclose(fp);
        sd_card_io_unlock();
        return;
    }

    uint8_t *file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) {
        ESP_LOGE(TAG, "Failed to alloc %ld bytes for MJPEG", file_size);
        fclose(fp);
        sd_card_io_unlock();
        return;
    }

    size_t actually_read = fread(file_buf, 1, file_size, fp);
    fclose(fp);
    sd_card_io_unlock();

    if ((long)actually_read != file_size) {
        ESP_LOGW(TAG, "MJPEG read incomplete: %u / %ld", (unsigned)actually_read, file_size);
        heap_caps_free(file_buf);
        return;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    if (jpeg_new_decoder_engine(&eng_cfg, &decoder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder");
        heap_caps_free(file_buf);
        return;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    TickType_t frame_ticks = pdMS_TO_TICKS(1000 / ANIM_FPS);
    int pos = 0;
    int total = (int)actually_read;

    while (atomic_load(&s_visible) && generation == atomic_load(&s_anim_generation) && pos < total - 1) {
        int soi = find_marker(file_buf + pos, total - pos, SOI_MARKER);
        if (soi < 0) break;
        soi += pos;
        int eoi = find_marker(file_buf + soi + 2, total - soi - 2, EOI_MARKER);
        if (eoi < 0) break;
        eoi += soi + 2;
        int frame_len = eoi + 2 - soi;

        TickType_t t0 = xTaskGetTickCount();
        uint32_t out_size = 0;
        esp_err_t ret = jpeg_decoder_process(decoder, &dec_cfg,
                                              file_buf + soi, frame_len,
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

        pos = eoi + 2;

        TickType_t elapsed = xTaskGetTickCount() - t0;
        if (elapsed < frame_ticks) {
            vTaskDelay(frame_ticks - elapsed);
        }
    }

    heap_caps_free(file_buf);
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

    s_time_label = lv_label_create(s_lock_page);
    lv_label_set_text(s_time_label, "12:00");
    lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, UI_VER_RES * 30 / 100);

    s_date_label = lv_label_create(s_lock_page);
    lv_label_set_text(s_date_label, "2026-01-01");
    lv_obj_set_style_text_color(s_date_label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_26, 0);
    lv_obj_align_to(s_date_label, s_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

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
    snprintf(buf, sizeof(buf), "%02d:%02d", st->hour, st->minute);
    lv_label_set_text(s_time_label, buf);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st->year, st->month, st->day);
    lv_label_set_text(s_date_label, buf);
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
     * 文件读取留给常驻 worker，不在显示/触摸回调中等待 SD 卡。 */
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
    /* 可由 SD worker 调用；只投递标量请求，不访问 LVGL 对象或文件。 */
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
    /* 熄屏/解锁后屏蔽 SD worker 迟到的开机动画请求；再次 show 才开放。 */
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
