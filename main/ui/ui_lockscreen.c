#include "ui_lockscreen.h"
#include "ui_statusbar.h"
#include "sd_card.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

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
static bool      s_visible    = true;
static bool      s_anim_running = false;

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

static void mjpeg_anim_task(void *arg)
{
    (void)arg;

    FILE *fp = fopen(MJPEG_PATH, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "No boot animation file: %s", MJPEG_PATH);
        s_anim_running = false;
        vTaskDelete(NULL);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "MJPEG file size invalid: %ld", file_size);
        fclose(fp);
        s_anim_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) {
        ESP_LOGE(TAG, "Failed to alloc %ld bytes for MJPEG", file_size);
        fclose(fp);
        s_anim_running = false;
        vTaskDelete(NULL);
        return;
    }

    size_t actually_read = fread(file_buf, 1, file_size, fp);
    fclose(fp);

    if ((long)actually_read != file_size) {
        ESP_LOGW(TAG, "MJPEG read incomplete: %u / %ld", (unsigned)actually_read, file_size);
        heap_caps_free(file_buf);
        s_anim_running = false;
        vTaskDelete(NULL);
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
        s_anim_running = false;
        vTaskDelete(NULL);
        return;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    TickType_t frame_ticks = pdMS_TO_TICKS(1000 / ANIM_FPS);
    int pos = 0;
    int total = (int)actually_read;

    while (s_visible && s_anim_running && pos < total - 1) {
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
            for (int row = 0; row < FRAME_H; row++) {
                memcpy(s_rgb_buf + row * FRAME_W * FRAME_BPP,
                       s_decode_buf + row * PADDED_W * FRAME_BPP,
                       FRAME_W * FRAME_BPP);
            }
            if (lvgl_port_lock(50)) {
                lv_canvas_set_buffer(s_bg_canvas, s_rgb_buf,
                                     FRAME_W, FRAME_H,
                                     LV_COLOR_FORMAT_RGB565);
                lv_obj_invalidate(s_bg_canvas);
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
    s_anim_running = false;
    ESP_LOGI(TAG, "Boot animation finished");
    vTaskDelete(NULL);
}

void ui_lockscreen_create(lv_obj_t *parent)
{
    s_lock_page = lv_obj_create(parent);
    lv_obj_set_size(s_lock_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_lock_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_lock_page, lv_color_hex(0x0D0D1A), 0);
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
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, UI_VER_RES * 30 / 100);

    s_date_label = lv_label_create(s_lock_page);
    lv_label_set_text(s_date_label, "2026-01-01");
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_26, 0);
    lv_obj_align_to(s_date_label, s_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    s_visible = true;
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
    s_anim_running = false;
    ui_statusbar_show_time(true);
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

void ui_lockscreen_show(void)
{
    if (!s_lock_page) return;
    lv_obj_remove_flag(s_lock_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_page);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock_page);
    lv_anim_set_values(&a, -(int32_t)UI_VER_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, show_anim_ready);
    lv_anim_start(&a);

    s_visible = true;
    ui_statusbar_show_time(false);
}

void ui_lockscreen_hide(void)
{
    if (!s_lock_page || !s_visible) return;
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
    if (s_anim_running || !s_rgb_buf || !s_decode_buf) return;
    s_anim_running = true;
    xTaskCreate(mjpeg_anim_task, "boot_anim", 8192, NULL, 5, NULL);
}
