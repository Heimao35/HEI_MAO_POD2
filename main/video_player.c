/**
 * @file video_player.c
 * @brief 裸 MJPEG 视频播放器后台 worker。
 *
 * 线程模型：
 * - UI 只把命令写入一个小队列；worker 独占 SD 文件、JPEG 解码器和帧缓冲。
 * - worker 在 LVGL 锁内同步调用 UI 回调，回调只更新控件，不做 I/O。
 * - 两帧 RGB565 缓冲交替使用：解码下一帧时，LVGL 可以继续显示上一帧。
 * - 进入播放前暂停音乐并关闭其 SD 文件；JPEG 硬件由 jpeg_codec 全局
 *   串行化，视频、封面和锁屏动画不再各自创建/删除 decoder engine。
 *
 * 60fps 策略：以 1/60 秒为节拍；如果显示/解码落后超过一帧，跳过下一帧
 * 的解码以追上节拍。面板 DPI 实际约 59.7Hz，因此 60fps 片源偶尔会少
 * 显示一帧，但没有音频，不需要音视频同步。
 */

#include "video_player.h"
#include "video_mjpeg.h"
#include "jpeg_codec.h"
#include "sd_card.h"
#include "music_player_adapter.h"
#include "audio_output.h"
#include "system_power.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "video_player";

#define VIDEO_FRAME_BYTES (VIDEO_PLAYER_PAD_WIDTH * VIDEO_PLAYER_PAD_HEIGHT * 2U)
#define VIDEO_THUMB_BYTES (VIDEO_PLAYER_THUMB_SIZE * VIDEO_PLAYER_THUMB_SIZE * 2U)
/* 460x460 baseline JPEG 通常远小于 512 KiB；缩小取帧窗口可减少缩略图
 * 持 SD 锁的时间。超过窗口的异常帧只会没有预览图，不影响播放。 */
#define VIDEO_THUMB_JPEG_MAX (512U * 1024U)
#define VIDEO_FRAME_US_DEFAULT (1000000U / 60U)
#define VIDEO_QUEUE_LEN   32
#define VIDEO_TASK_STACK  16384
#define VIDEO_TASK_PRIO   5
#define VIDEO_SD_LOCK_MS  1000

typedef enum {
    CMD_SCAN = 1,
    CMD_PAGE,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_TOGGLE,
    CMD_SEEK,
    CMD_STOP,
    CMD_RELEASE,
} video_cmd_type_t;

typedef struct {
    uint8_t type;
    uint32_t arg;   /* 视频索引或页首索引 */
    uint16_t arg16; /* 0..1000 的进度 */
    uint32_t epoch; /* 入队时的会话代次；STOP/RELEASE/PLAY 会递增 */
} video_cmd_t;

/* UI 回调与 worker 状态。回调指针在启动时注册一次，之后只读。 */
static video_player_ops_t s_ops;
static void *s_ops_ctx;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static atomic_uint s_state;
static atomic_uint s_index;
static atomic_uint s_progress;
static atomic_bool s_playing_flag;
static atomic_bool s_reader_open;
static atomic_uint s_pending;
static atomic_bool s_cancel;
static atomic_bool s_stop_request;
static atomic_bool s_release_request;
/* STOP/RELEASE/PLAY 递增会话代次，worker 丢弃代次不匹配的旧命令。
 * 这样“先排队 PAGE，再离开模式”不会在锁屏/音乐模式后台重新生成缩略图；
 * 而“离开后马上重新进入”的新 PAGE 使用新代次，仍会执行。 */
static atomic_uint s_work_epoch;
static atomic_uint s_page_token;

/* 以下变量只由 worker 访问。 */
static video_mjpeg_t *s_reader;
static uint8_t *s_frames[2];
static uint8_t s_frame_index;
static uint32_t s_frame_seq;
static uint32_t s_play_index;
static bool s_paused;
static bool s_ended;
static bool s_error;
static bool s_skip_next;
static int64_t s_deadline;
/* 当前视频的目标帧周期；由文件名中的 24fps/30fps/60fps 标记决定，
 * 无标记时默认 60fps。 */
static uint32_t s_frame_us = VIDEO_FRAME_US_DEFAULT;
static bool s_music_paused;
static uint32_t s_library_generation;
static bool s_library_valid;
static uint8_t *s_thumb_jpeg;
static uint8_t *s_thumb_decode;
static uint8_t *s_thumb_pixels[VIDEO_PAGE_SIZE];

/* 5 秒一次的解码诊断。它只统计 worker 自己观测到的读/解码耗时和丢帧，
 * 不是整机 CPU 占用，也不代表面板实际扫描帧率。 */
static uint32_t s_diag_frames;
static uint32_t s_diag_drops;
static uint32_t s_diag_reads;
static uint64_t s_diag_bytes;
static uint32_t s_diag_decode_us;
static uint32_t s_diag_decode_max_us;
static uint32_t s_diag_lock_us;
static uint32_t s_diag_lock_max_us;
static uint32_t s_diag_io_us;
static uint32_t s_diag_io_max_us;
static int64_t s_diag_start_us;

/* ------------------------------------------------------------------------- */
/* UI 回调：全部在 LVGL 锁内执行，失败时跳过本帧/本次通知，不阻塞 worker。 */

static void publish_state(video_state_t state, uint32_t index, const char *message)
{
    atomic_store(&s_state, (unsigned)state);
    atomic_store(&s_index, index);
    if (s_ops.state && lvgl_port_lock(100)) {
        s_ops.state(state, index, message, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void publish_status(const char *text)
{
    if (s_ops.status && lvgl_port_lock(100)) {
        s_ops.status(text, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void publish_library(uint32_t count, bool truncated, uint32_t generation)
{
    if (s_ops.library && lvgl_port_lock(100)) {
        s_ops.library(count, truncated, generation, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void publish_page(const video_page_t *page, uint32_t generation)
{
    if (s_ops.page && lvgl_port_lock(100)) {
        s_ops.page(page, generation, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void publish_thumbnail(uint32_t slot, uint32_t index, const uint8_t *pixels,
                              uint16_t w, uint16_t h, uint32_t generation)
{
    /* 释放缓冲前发送的 NULL 通知必须真正送达 UI，不能因为锁超时留下
     * 悬空的 lv_image 数据指针。 */
    TickType_t timeout = pixels ? pdMS_TO_TICKS(100) : portMAX_DELAY;
    if (s_ops.thumbnail && lvgl_port_lock(timeout)) {
        s_ops.thumbnail(slot, index, pixels, w, h, generation, s_ops_ctx);
        lvgl_port_unlock();
    }
}

/* 传入 NULL 表示让 UI 清除 canvas，随后 worker 才能安全释放帧缓冲。 */
static void publish_frame(const uint8_t *pixels)
{
    ++s_frame_seq;
    /* 正常帧最多等 50ms，避免 UI 忙时把解码线程拖死；清除 canvas 的
     * NULL 通知必须等待 LVGL 锁，否则释放帧缓冲后 canvas 仍可能读到它。 */
    TickType_t timeout = pixels ? pdMS_TO_TICKS(50) : portMAX_DELAY;
    if (s_ops.frame && lvgl_port_lock(timeout)) {
        s_ops.frame(pixels, pixels ? VIDEO_PLAYER_PAD_WIDTH : 0,
                    pixels ? VIDEO_PLAYER_PAD_HEIGHT : 0,
                    s_frame_seq, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void update_progress(void)
{
    if (!s_reader) return;
    uint64_t size = video_mjpeg_size(s_reader);
    uint64_t pos = video_mjpeg_tell(s_reader);
    uint32_t per_mille = size ? (uint32_t)(pos * 1000U / size) : 0;
    if (per_mille > 1000) per_mille = 1000;
    atomic_store(&s_progress, per_mille);
}

/* ------------------------------------------------------------------------- */
/* 资源生命周期。所有 JPEG 缓冲都放 PSRAM，内部 DMA 只保留驱动自己的描述符。 */

static void free_thumb_buffers(void);
static void scan_library(void);
static void video_diag_reset(void);

static esp_err_t ensure_frames(void)
{
    if (s_frames[0] && s_frames[1]) return ESP_OK;
    jpeg_decode_memory_alloc_cfg_t mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    for (unsigned i = 0; i < 2; ++i) {
        size_t actual = 0;
        s_frames[i] = jpeg_alloc_decoder_mem(VIDEO_FRAME_BYTES, &mem, &actual);
        if (!s_frames[i] || actual < VIDEO_FRAME_BYTES) {
            heap_caps_free(s_frames[0]);
            heap_caps_free(s_frames[1]);
            s_frames[0] = s_frames[1] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void free_frames(void)
{
    publish_frame(NULL);
    heap_caps_free(s_frames[0]);
    heap_caps_free(s_frames[1]);
    s_frames[0] = s_frames[1] = NULL;
    s_frame_index = 0;
}

static esp_err_t ensure_thumb_buffers(void)
{
    bool complete = s_thumb_jpeg && s_thumb_decode;
    for (unsigned i = 0; complete && i < VIDEO_PAGE_SIZE; ++i)
        complete = s_thumb_pixels[i] != NULL;
    if (complete) return ESP_OK;

    /* 如果上一次只分配了一部分，先统一释放，避免重复分配泄漏。 */
    free_thumb_buffers();
    s_thumb_jpeg = heap_caps_aligned_alloc(VIDEO_MJPEG_IO_ALIGN,
                                           VIDEO_THUMB_JPEG_MAX,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_thumb_jpeg)
        s_thumb_jpeg = heap_caps_malloc(VIDEO_THUMB_JPEG_MAX,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_decode_memory_alloc_cfg_t mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual = 0;
    s_thumb_decode = jpeg_alloc_decoder_mem(VIDEO_FRAME_BYTES, &mem, &actual);
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        s_thumb_pixels[i] = heap_caps_malloc(VIDEO_THUMB_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_thumb_jpeg || !s_thumb_decode || actual < VIDEO_FRAME_BYTES) {
        goto fail;
    }
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        if (!s_thumb_pixels[i]) goto fail;
    }
    return ESP_OK;
fail:
    heap_caps_free(s_thumb_jpeg);
    heap_caps_free(s_thumb_decode);
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        heap_caps_free(s_thumb_pixels[i]);
        s_thumb_pixels[i] = NULL;
    }
    s_thumb_jpeg = NULL;
    s_thumb_decode = NULL;
    return ESP_ERR_NO_MEM;
}

static void free_thumb_buffers(void)
{
    /* 先让 UI 解除对四个缩略图缓冲的引用，再释放 PSRAM。 */
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i)
        publish_thumbnail(i, UINT32_MAX, NULL, 0, 0, 0);
    heap_caps_free(s_thumb_jpeg);
    s_thumb_jpeg = NULL;
    heap_caps_free(s_thumb_decode);
    s_thumb_decode = NULL;
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        heap_caps_free(s_thumb_pixels[i]);
        s_thumb_pixels[i] = NULL;
    }
}

static bool decode_frame(const uint8_t *jpeg, size_t bytes, uint8_t *out)
{
    if (!jpeg || !bytes || !out) return false;
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* 与锁屏一致：BGR 选择 LVGL 需要的原生小端 RGB565。 */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t out_size = 0;
    esp_err_t ret = jpeg_codec_process(&cfg, jpeg, (uint32_t)bytes,
                                       out, VIDEO_FRAME_BYTES, &out_size, 1000);
    return ret == ESP_OK && out_size == VIDEO_FRAME_BYTES;
}

/* ------------------------------------------------------------------------- */
/* SD 文件与音频交接。 */

static void close_reader(void)
{
    video_mjpeg_t *reader = s_reader;
    if (!reader) {
        atomic_store(&s_reader_open, false);
        return;
    }
    /* SD 卸载任务会先调用 video_detach_cb() 关闭 FILE；此时即使拿不到
     * SD 锁，也可以只释放 reader 的 PSRAM。 */
    if (sd_card_io_lock(VIDEO_SD_LOCK_MS)) {
        video_mjpeg_close(reader);
        sd_card_io_unlock();
    } else {
        video_mjpeg_detach(reader);
        video_mjpeg_close(reader);
    }
    s_reader = NULL;
    atomic_store(&s_reader_open, false);
}

static uint32_t frame_period_from_name(const char *name)
{
    uint8_t fps = video_mjpeg_fps_from_name(name);
    if (fps == 24) return 1000000U / 24U;
    if (fps == 30) return 1000000U / 30U;
    return 1000000U / 60U;
}

static esp_err_t open_reader(uint32_t index, char *err, size_t err_size)
{
    video_item_t item;
    char path[VIDEO_NAME_MAX + 80];

    if (!sd_card_is_mounted() || !sd_card_is_present()) {
        snprintf(err, err_size, "SD 卡不可用");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_library_valid || s_library_generation != sd_card_get_generation()) {
        snprintf(err, err_size, "视频列表已过期，请重新扫描");
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) {
        snprintf(err, err_size, "SD 卡忙，请稍后重试");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = video_library_get_item(index, &item);
    if (ret == ESP_OK) {
        s_frame_us = frame_period_from_name(item.name);
        ret = video_library_make_path_name(item.name, path, sizeof(path));
    }
    if (ret == ESP_OK) ret = video_mjpeg_open(&s_reader, path);
    sd_card_io_unlock();

    if (ret != ESP_OK) {
        snprintf(err, err_size, "无法打开视频文件");
        return ret;
    }
    ESP_LOGI(TAG, "Opened video: %llu bytes, target=%u fps (%u us/frame)",
             (unsigned long long)video_mjpeg_size(s_reader),
             (unsigned)(1000000U / s_frame_us), (unsigned)s_frame_us);
    atomic_store(&s_reader_open, true);
    return ESP_OK;
}

static bool read_frame(const uint8_t **frame, size_t *bytes, uint64_t *offset)
{
    if (!s_reader || !video_mjpeg_valid(s_reader)) return false;

    int64_t lock_start = esp_timer_get_time();
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) return false;
    int64_t io_start = esp_timer_get_time();
    bool ok = video_mjpeg_next(s_reader, frame, bytes, offset);
    int64_t io_end = esp_timer_get_time();
    sd_card_io_unlock();

    uint32_t lock_us = (uint32_t)(io_start - lock_start);
    uint32_t io_us = (uint32_t)(io_end - io_start);
    s_diag_lock_us += lock_us;
    s_diag_io_us += io_us;
    if (lock_us > s_diag_lock_max_us) s_diag_lock_max_us = lock_us;
    if (io_us > s_diag_io_max_us) s_diag_io_max_us = io_us;
    if (ok) {
        ++s_diag_reads;
        s_diag_bytes += *bytes;
    }
    return ok;
}

static bool seek_reader(uint64_t offset)
{
    if (!s_reader || !video_mjpeg_valid(s_reader)) return false;
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) return false;
    bool ok = video_mjpeg_seek(s_reader, offset);
    sd_card_io_unlock();
    return ok;
}

/*
 * 视频页要求“不进行音频解码”。先投递暂停，再等待媒体 worker/音频
 * 关闭 SD 文件；最多等待约 1 秒，失败则拒绝开始播放，避免边放视频
 * 边有音乐解码争用 CPU 和 SD 锁。
 */
static bool pause_music_for_video(void)
{
    if (s_music_paused) return true;
    audio_output_set_playing(false);
    s_music_paused = true;
    /* 等媒体 worker 完成当前曲库扫描/封面读取，避免它和视频每帧争 SD 锁。
     * 3 秒仍不空闲时继续，至少音频已经停止；日志会暴露剩余占用。 */
    for (unsigned i = 0; i < 150; ++i) {
        if (atomic_load(&s_cancel)) return false;
        if (music_player_adapter_pause(true)) {
            if (i) ESP_LOGI(TAG, "Music paused after %u ms", i * 20U);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "Music media worker still busy after 3s; video read may contend for SD");
    return true;
}

static void resume_music_after_video(void)
{
    if (!s_music_paused) return;
    s_music_paused = false;
    /* 关机/低电流程自己持有媒体暂停位，视频退出不能把它解除。 */
    if (!system_power_is_active()) music_player_adapter_pause(false);
}

/* ------------------------------------------------------------------------- */
/* 缩略图：取文件中间附近的一帧，解码后box filter缩到176x176。 */

static void downscale_thumbnail(const uint8_t *src, uint8_t *dst)
{
    const uint16_t *s = (const uint16_t *)src;
    uint16_t *d = (uint16_t *)dst;
    const unsigned sw = VIDEO_PLAYER_WIDTH;
    const unsigned sh = VIDEO_PLAYER_HEIGHT;
    const unsigned dw = VIDEO_PLAYER_THUMB_SIZE;
    const unsigned dh = VIDEO_PLAYER_THUMB_SIZE;

    for (unsigned y = 0; y < dh; ++y) {
        unsigned y0 = y * sh / dh;
        unsigned y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (unsigned x = 0; x < dw; ++x) {
            unsigned x0 = x * sw / dw;
            unsigned x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                const uint16_t *row = s + sy * VIDEO_PLAYER_PAD_WIDTH;
                for (unsigned sx = x0; sx < x1; ++sx) {
                    uint16_t p = row[sx];
                    r += (p >> 11) & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    ++n;
                }
            }
            if (!n) n = 1;
            d[y * dw + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

static bool make_thumbnail(uint32_t slot, uint32_t index, const char *name,
                           uint32_t file_size, uint32_t generation, uint32_t token)
{
    if (slot >= VIDEO_PAGE_SIZE || !s_thumb_jpeg || !s_thumb_decode ||
        !s_thumb_pixels[slot]) return false;
    char path[VIDEO_NAME_MAX + 80];
    if (video_library_make_path_name(name, path, sizeof(path)) != ESP_OK) return false;

    size_t jpeg_bytes = 0;
    uint64_t jpeg_offset = 0;
    /* 索引不保存文件大小；UINT64_MAX 让取帧函数先 stat，再选中间位置。 */
    uint64_t target = file_size ? (uint64_t)file_size / 2 : UINT64_MAX;
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) return false;
    esp_err_t ret = video_mjpeg_read_frame_near(path, target, s_thumb_jpeg,
                                                 VIDEO_THUMB_JPEG_MAX,
                                                 &jpeg_bytes, &jpeg_offset);
    sd_card_io_unlock();
    if (ret != ESP_OK || atomic_load(&s_cancel) ||
        atomic_load(&s_page_token) != token) return false;

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(s_thumb_jpeg, (uint32_t)jpeg_bytes, &info) != ESP_OK ||
        info.width != VIDEO_PLAYER_WIDTH || info.height != VIDEO_PLAYER_HEIGHT)
        return false;
    if (!decode_frame(s_thumb_jpeg, jpeg_bytes, s_thumb_decode)) return false;
    if (atomic_load(&s_cancel) || atomic_load(&s_page_token) != token) return false;

    downscale_thumbnail(s_thumb_decode, s_thumb_pixels[slot]);
    publish_thumbnail(slot, index, s_thumb_pixels[slot],
                      VIDEO_PLAYER_THUMB_SIZE, VIDEO_PLAYER_THUMB_SIZE, generation);
    return true;
}

static bool priority_command_pending(void)
{
    video_cmd_t cmd;
    if (xQueuePeek(s_queue, &cmd, 0) != pdTRUE) return false;
    return cmd.type == CMD_PLAY || cmd.type == CMD_STOP ||
           cmd.type == CMD_RELEASE || cmd.type == CMD_PAGE;
}

static void load_page(uint32_t first)
{
    /* 如果 STOP/RELEASE 已经投递但尚未处理，不能再启动新的缩略图工作；
     * 否则锁屏会等待这部分 SD/JPEG 任务。否则开始新一页时重新开放
     * cancel，加载途中的停止请求仍会再次置位。 */
    if (atomic_load(&s_stop_request) || atomic_load(&s_release_request)) return;
    atomic_store(&s_cancel, false);
    if (!s_library_valid) {
        /* 锁屏/换卡可能中断上一次扫描，索引已经不可信。页面请求在这里
         * 自动补一次扫描；成功后 library 回调会让 UI 重新请求本页。 */
        scan_library();
        return;
    }
    if (s_library_generation != sd_card_get_generation()) {
        publish_status("视频列表已过期，请重新扫描");
        return;
    }
    uint32_t count = video_library_get_count();
    if (first >= count) {
        publish_status("视频列表为空");
        return;
    }
    if (ensure_thumb_buffers() != ESP_OK) ESP_LOGW(TAG, "Thumbnail buffers unavailable");

    video_page_t page;
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) {
        publish_status("SD 卡忙，请稍后重试");
        return;
    }
    esp_err_t ret = video_library_get_page(first, &page);
    uint32_t generation = sd_card_get_generation();
    sd_card_io_unlock();
    if (ret != ESP_OK) {
        publish_status("读取视频列表失败");
        return;
    }
    if (generation != s_library_generation) {
        publish_status("视频列表已过期，请重新扫描");
        return;
    }

    uint32_t token = atomic_load(&s_page_token);
    publish_page(&page, generation);
    if (!s_thumb_jpeg || !s_thumb_decode) return;

    for (uint32_t slot = 0; slot < page.count; ++slot) {
        if (atomic_load(&s_cancel) || atomic_load(&s_page_token) != token ||
            priority_command_pending()) break;
        make_thumbnail(slot, first + slot, page.names[slot], page.sizes[slot],
                       generation, token);
    }
}

static void scan_library(void)
{
    if (s_library_valid && s_library_generation == sd_card_get_generation()) {
        publish_library(video_library_get_count(), video_library_is_truncated(),
                        s_library_generation);
        return;
    }

    if (atomic_load(&s_stop_request) || atomic_load(&s_release_request)) return;
    publish_status("正在扫描视频...");
    atomic_store(&s_cancel, false);
    if (!sd_card_io_lock(VIDEO_SD_LOCK_MS)) {
        /* 锁都没拿到时不能丢失旧索引的有效性。 */
        publish_status("SD 卡忙，请稍后重试");
        return;
    }
    /* video_library_scan 会重写索引文件；从调用前起旧列表不再可信，
     * 成功后才重新发布。 */
    s_library_valid = false;
    s_library_generation = 0;
    esp_err_t ret = video_library_scan(&s_cancel);
    uint32_t generation = sd_card_get_generation();
    sd_card_io_unlock();
    if (ret != ESP_OK || atomic_load(&s_cancel)) {
        if (ret != ESP_ERR_INVALID_STATE)
            publish_status("视频扫描失败");
        return;
    }
    s_library_generation = generation;
    s_library_valid = true;
    publish_library(video_library_get_count(), video_library_is_truncated(), generation);
}

/* ------------------------------------------------------------------------- */
/* 播放。 */

static void reset_playback_flags(void)
{
    s_paused = false;
    s_ended = false;
    s_error = false;
    s_skip_next = false;
    s_deadline = 0;
}

static void stop_session(bool release_all, bool notify_idle)
{
    ++s_page_token; /* 让正在进行的缩略图循环尽快退出 */
    close_reader();
    atomic_store(&s_playing_flag, false);
    reset_playback_flags();
    free_frames();
    if (release_all) {
        free_thumb_buffers();
    }
    if (notify_idle) {
        publish_state(VIDEO_STATE_IDLE, UINT32_MAX, NULL);
        resume_music_after_video();
    }
    atomic_store(&s_stop_request, false);
    atomic_store(&s_release_request, false);
}

static esp_err_t prepare_play(uint32_t index, char *err, size_t err_size)
{
    if (index >= video_library_get_count()) {
        snprintf(err, err_size, "视频不存在");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_library_valid || s_library_generation != sd_card_get_generation()) {
        snprintf(err, err_size, "视频列表已过期，请重新扫描");
        return ESP_ERR_INVALID_STATE;
    }

    ++s_page_token;
    if (s_reader) close_reader();
    reset_playback_flags();
    video_diag_reset();
    s_frame_us = VIDEO_FRAME_US_DEFAULT;
    atomic_store(&s_progress, 0);
    s_play_index = index;
    atomic_store(&s_playing_flag, true);
    publish_state(VIDEO_STATE_LOADING, index, NULL);

    if (!pause_music_for_video()) {
        snprintf(err, err_size, "无法暂停音乐播放，请稍后重试");
        return ESP_FAIL;
    }
    if (atomic_load(&s_cancel)) {
        snprintf(err, err_size, "播放已取消");
        return ESP_ERR_INVALID_STATE;
    }
    if (ensure_frames() != ESP_OK) {
        snprintf(err, err_size, "帧缓冲内存不足");
        return ESP_ERR_NO_MEM;
    }
    if (open_reader(index, err, err_size) != ESP_OK) return ESP_FAIL;
    if (atomic_load(&s_cancel)) {
        snprintf(err, err_size, "播放已取消");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *frame = NULL;
    size_t bytes = 0;
    uint64_t offset = 0;
    if (!read_frame(&frame, &bytes, &offset)) {
        const char *reason = video_mjpeg_at_end(s_reader) ?
                             "视频没有完整的 460x460 帧" : "读取视频帧失败";
        snprintf(err, err_size, "%s", reason);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "First frame: %u bytes", (unsigned)bytes);
    /* 始终写当前未显示的那块缓冲：切换视频时旧 canvas 可能仍指向
     * s_frames[s_frame_index]，不能在它上面解码。 */
    uint8_t *first = s_frames[s_frame_index ^ 1];
    if (!decode_frame(frame, bytes, first)) {
        snprintf(err, err_size, "帧解码失败，需要 460x460 baseline MJPEG");
        return ESP_FAIL;
    }
    s_frame_index ^= 1;
    publish_frame(s_frames[s_frame_index]);
    update_progress();
    publish_state(VIDEO_STATE_PLAYING, index, NULL);
    s_deadline = esp_timer_get_time() + s_frame_us;
    return ESP_OK;
}

static void fail_playback(const char *message)
{
    close_reader();
    atomic_store(&s_playing_flag, false);
    s_error = true;
    free_frames();
    publish_state(VIDEO_STATE_ERROR, s_play_index, message);
}

static void start_play(uint32_t index)
{
    char err[64] = {0};
    if (atomic_load(&s_stop_request) || atomic_load(&s_release_request)) return;
    atomic_store(&s_cancel, false);

    esp_err_t ret = prepare_play(index, err, sizeof(err));
    if (ret == ESP_OK) return;
    if (atomic_load(&s_cancel)) {
        /* 被锁屏/关机取消：不显示错误页，交给排队的 STOP/RELEASE 清理。 */
        close_reader();
        atomic_store(&s_playing_flag, false);
        free_frames();
        publish_state(VIDEO_STATE_IDLE, index, NULL);
        return;
    }
    fail_playback(err[0] ? err : "视频播放失败");
}

static void decode_one_paused_frame(void)
{
    const uint8_t *frame = NULL;
    size_t bytes = 0;
    uint64_t offset = 0;
    if (!read_frame(&frame, &bytes, &offset)) {
        if (s_reader && video_mjpeg_at_end(s_reader)) {
            /* 拖到 100% 时没有下一帧是正常结束，不显示错误页。 */
            s_ended = true;
            atomic_store(&s_playing_flag, false);
            atomic_store(&s_progress, 1000);
            publish_state(VIDEO_STATE_ENDED, s_play_index, NULL);
        } else {
            fail_playback("读取视频帧失败");
        }
        return;
    }
    uint8_t *dst = s_frames[s_frame_index ^ 1];
    if (!decode_frame(frame, bytes, dst)) {
        fail_playback("帧解码失败，需要 460x460 baseline MJPEG");
        return;
    }
    s_frame_index ^= 1;
    publish_frame(s_frames[s_frame_index]);
    update_progress();
}

static bool seek_to(uint16_t per_mille)
{
    if (!s_reader) return false;
    uint64_t size = video_mjpeg_size(s_reader);
    uint64_t target = size ? (size * (uint64_t)per_mille / 1000U) : 0;
    if (target >= size) target = size ? size - 1 : 0;
    if (!seek_reader(target)) return false;
    s_skip_next = false;
    atomic_store(&s_progress, per_mille > 1000 ? 1000 : per_mille);
    return true;
}

static void resume_playback(void)
{
    if (s_ended) {
        /* ENDED 时文件仍然打开；从头跳转并继续。 */
        if (!seek_to(0)) {
            fail_playback("无法重新开始视频");
            return;
        }
        s_ended = false;
    }
    s_paused = false;
    atomic_store(&s_playing_flag, true);
    video_diag_reset(); /* 暂停时长不计入恢复后的 fps */
    s_deadline = esp_timer_get_time() + s_frame_us;
    s_skip_next = false;
    publish_state(VIDEO_STATE_PLAYING, s_play_index, NULL);
}

static void video_diag_reset(void)
{
    s_diag_frames = s_diag_drops = 0;
    s_diag_reads = 0;
    s_diag_bytes = 0;
    s_diag_decode_us = 0;
    s_diag_decode_max_us = 0;
    s_diag_lock_us = s_diag_lock_max_us = 0;
    s_diag_io_us = s_diag_io_max_us = 0;
    s_diag_start_us = 0;
}

static void video_diag_tick(int64_t now)
{
    if (!s_diag_start_us) {
        s_diag_start_us = now;
        return;
    }
    int64_t elapsed = now - s_diag_start_us;
    if (elapsed < 5000000) return;
    uint32_t frames = s_diag_frames;
    uint32_t reads = s_diag_reads;
    if (frames && reads) {
        uint32_t fps = (uint32_t)((uint64_t)frames * 1000000U / (uint64_t)elapsed);
        ESP_LOGI(TAG,
                 "Video: %lu decoded fps (target %lu), drops=%lu, frame avg=%lu B, "
                 "lock avg/max=%lu/%lu us, io avg/max=%lu/%lu us, "
                 "decode avg/max=%lu/%lu us",
                 (unsigned long)fps,
                 (unsigned long)(1000000U / s_frame_us),
                 (unsigned long)s_diag_drops,
                 (unsigned long)(s_diag_bytes / reads),
                 (unsigned long)(s_diag_lock_us / reads),
                 (unsigned long)s_diag_lock_max_us,
                 (unsigned long)(s_diag_io_us / reads),
                 (unsigned long)s_diag_io_max_us,
                 (unsigned long)(s_diag_decode_us / frames),
                 (unsigned long)s_diag_decode_max_us);
    }
    s_diag_frames = s_diag_drops = 0;
    s_diag_reads = 0;
    s_diag_bytes = 0;
    s_diag_decode_us = 0;
    s_diag_decode_max_us = 0;
    s_diag_lock_us = s_diag_lock_max_us = 0;
    s_diag_io_us = s_diag_io_max_us = 0;
    s_diag_start_us = now;
}

static void frame_step(void)
{
    if (!atomic_load(&s_playing_flag) || s_paused || s_ended || s_error) return;

    int64_t now = esp_timer_get_time();
    if (now < s_deadline) return;
    if (now > s_deadline + s_frame_us) {
        /* 已经落后超过一帧：丢掉下一帧的解码，只保留时间轴。 */
        s_skip_next = true;
        s_deadline = now;
    }

    const uint8_t *frame = NULL;
    size_t bytes = 0;
    uint64_t offset = 0;
    if (!read_frame(&frame, &bytes, &offset)) {
        if (!sd_card_is_present()) {
            fail_playback("SD 卡已移除");
        } else if (s_reader && video_mjpeg_at_end(s_reader)) {
            atomic_store(&s_playing_flag, false);
            s_ended = true;
            atomic_store(&s_progress, 1000);
            publish_state(VIDEO_STATE_ENDED, s_play_index, NULL);
        } else {
            fail_playback("读取视频帧失败");
        }
        return;
    }
    update_progress();
    if (s_skip_next) {
        s_skip_next = false;
        ++s_diag_drops;
        video_diag_tick(esp_timer_get_time());
        return;
    }

    uint8_t *dst = s_frames[s_frame_index ^ 1];
    int64_t decode_start = esp_timer_get_time();
    if (!decode_frame(frame, bytes, dst)) {
        fail_playback("帧解码失败，需要 460x460 baseline MJPEG");
        return;
    }
    uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start);
    s_diag_decode_us += decode_us;
    if (decode_us > s_diag_decode_max_us) s_diag_decode_max_us = decode_us;
    ++s_diag_frames;
    s_frame_index ^= 1;
    publish_frame(s_frames[s_frame_index]);
    update_progress();

    s_deadline += s_frame_us;
    now = esp_timer_get_time();
    if (now > s_deadline + s_frame_us) {
        s_skip_next = true;
        s_deadline = now;
    }
    video_diag_tick(now);
}

/* ------------------------------------------------------------------------- */
/* 命令处理。 */

static void handle_command(const video_cmd_t *cmd)
{
    bool current = cmd->epoch == atomic_load(&s_work_epoch);
    switch (cmd->type) {
    case CMD_SCAN:
        if (!current || atomic_load(&s_playing_flag)) break;
        ++s_page_token;
        scan_library();
        break;
    case CMD_PAGE:
        if (!current || atomic_load(&s_playing_flag)) break;
        ++s_page_token;
        load_page(cmd->arg);
        break;
    case CMD_PLAY:
        if (!current) break;
        start_play(cmd->arg);
        break;
    case CMD_PAUSE:
        if (!current) break;
        if (atomic_load(&s_playing_flag) && !s_ended && !s_error) {
            s_paused = true;
            publish_state(VIDEO_STATE_PAUSED, s_play_index, NULL);
        }
        break;
    case CMD_RESUME:
        if (!current) break;
        if (!atomic_load(&s_playing_flag) && !s_ended && !s_error) break;
        resume_playback();
        break;
    case CMD_TOGGLE:
        if (!current) break;
        if (s_paused || s_ended) resume_playback();
        else if (atomic_load(&s_playing_flag) && !s_error) {
            s_paused = true;
            publish_state(VIDEO_STATE_PAUSED, s_play_index, NULL);
        }
        break;
    case CMD_SEEK: {
        if (!current || !s_reader || s_error) break;
        if (!seek_to(cmd->arg16)) {
            publish_status("无法跳转");
            break;
        }
        if (s_ended) {
            s_ended = false;
            s_paused = true;
            atomic_store(&s_playing_flag, true);
        }
        if (s_paused) {
            decode_one_paused_frame();
            if (!s_error) publish_state(VIDEO_STATE_PAUSED, s_play_index, NULL);
        } else {
            s_deadline = esp_timer_get_time() + s_frame_us;
            publish_state(VIDEO_STATE_PLAYING, s_play_index, NULL);
        }
        break;
    }
    case CMD_STOP:
        stop_session(false, true);
        break;
    case CMD_RELEASE:
        stop_session(true, true);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* worker 与公开 API。 */

static void video_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 停止/释放请求优先处理：即使命令队列满，也能从解码循环退出。 */
        if (atomic_exchange(&s_release_request, false)) {
            stop_session(true, true);
            continue;
        }
        if (atomic_exchange(&s_stop_request, false)) {
            stop_session(false, true);
            continue;
        }

        TickType_t wait = portMAX_DELAY;
        if (atomic_load(&s_playing_flag)) {
            if (s_paused || s_ended || s_error) {
                wait = pdMS_TO_TICKS(100);
            } else {
                int64_t now = esp_timer_get_time();
                if (now < s_deadline) {
                    uint32_t ms = (uint32_t)((s_deadline - now + 999) / 1000);
                    wait = pdMS_TO_TICKS(ms ? ms : 1);
                } else {
                    wait = 0;
                }
            }
        }

        video_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, wait) == pdTRUE) {
            handle_command(&cmd);
            atomic_fetch_sub(&s_pending, 1);
            continue;
        }

        if (!atomic_load(&s_playing_flag) || s_paused || s_ended || s_error)
            continue;
        frame_step();
    }
}

static bool enqueue(uint8_t type, uint32_t arg, uint16_t arg16)
{
    if (!s_queue) return false;
    video_cmd_t cmd = {
        .type = type,
        .arg = arg,
        .arg16 = arg16,
        .epoch = atomic_load(&s_work_epoch),
    };
    atomic_fetch_add(&s_pending, 1);
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        atomic_fetch_sub(&s_pending, 1);
        ESP_LOGW(TAG, "Command queue full; type=%u dropped", type);
        return false;
    }
    return true;
}

static void video_detach_cb(void)
{
    /* sd_card 已持有 SD 锁；这里只关闭 FILE，不能再次取锁。 */
    if (s_reader) video_mjpeg_detach(s_reader);
}

esp_err_t video_player_init(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    atomic_init(&s_state, VIDEO_STATE_IDLE);
    atomic_init(&s_index, UINT32_MAX);
    atomic_init(&s_progress, 0);
    atomic_init(&s_playing_flag, false);
    atomic_init(&s_reader_open, false);
    atomic_init(&s_pending, 0);
    atomic_init(&s_cancel, false);
    atomic_init(&s_stop_request, false);
    atomic_init(&s_release_request, false);
    atomic_init(&s_work_epoch, 0);
    atomic_init(&s_page_token, 0);

    s_queue = xQueueCreate(VIDEO_QUEUE_LEN, sizeof(video_cmd_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(video_task, "video", VIDEO_TASK_STACK, NULL,
                                VIDEO_TASK_PRIO, &s_task, 0) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = sd_card_set_video_detach_callback(video_detach_cb);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Video SD detach callback unavailable: %s",
                                esp_err_to_name(ret));
    return ESP_OK;
}

void video_player_set_ops(const video_player_ops_t *ops, void *ctx)
{
    s_ops = ops ? *ops : (video_player_ops_t){0};
    s_ops_ctx = ctx;
}

void video_player_scan(void)
{
    atomic_fetch_add(&s_page_token, 1);
    enqueue(CMD_SCAN, 0, 0);
}

void video_player_load_page(uint32_t first)
{
    atomic_fetch_add(&s_page_token, 1);
    enqueue(CMD_PAGE, first, 0);
}

void video_player_play(uint32_t index)
{
    /* 新播放会话：丢弃队列里属于旧会话的暂停/拖动/翻页命令。 */
    atomic_fetch_add(&s_work_epoch, 1);
    atomic_store(&s_cancel, false);
    enqueue(CMD_PLAY, index, 0);
}

void video_player_pause(void)
{
    enqueue(CMD_PAUSE, 0, 0);
}

void video_player_resume(void)
{
    enqueue(CMD_RESUME, 0, 0);
}

void video_player_toggle(void)
{
    enqueue(CMD_TOGGLE, 0, 0);
}

void video_player_seek(uint16_t per_mille)
{
    if (per_mille > 1000) per_mille = 1000;
    enqueue(CMD_SEEK, 0, per_mille);
}

void video_player_stop(void)
{
    atomic_fetch_add(&s_work_epoch, 1);
    atomic_store(&s_cancel, true);
    atomic_store(&s_stop_request, true);
    enqueue(CMD_STOP, 0, 0);
}

void video_player_release(void)
{
    atomic_fetch_add(&s_work_epoch, 1);
    atomic_store(&s_cancel, true);
    atomic_store(&s_release_request, true);
    enqueue(CMD_RELEASE, 0, 0);
}

bool video_player_is_active(void)
{
    return video_player_get_state() != VIDEO_STATE_IDLE;
}

bool video_player_is_idle(void)
{
    if (!s_task) return true;
    return atomic_load(&s_pending) == 0 &&
           !atomic_load(&s_playing_flag) &&
           !atomic_load(&s_reader_open);
}

video_state_t video_player_get_state(void)
{
    return (video_state_t)atomic_load(&s_state);
}

uint32_t video_player_get_index(void)
{
    return atomic_load(&s_index);
}

uint16_t video_player_get_progress(void)
{
    return (uint16_t)atomic_load(&s_progress);
}
