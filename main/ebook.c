/**
 * @file ebook.c
 * @brief 电子书后台 worker：扫描 BOOKS、封面解码、UTF-8 TXT 打开/分页/取页。
 *
 * 线程模型与 image_viewer 一致：UI 只投递请求；worker 独占 SD/JPEG/文本
 * 缓冲，并在 LVGL 锁内同步回调 UI。回调里不能等待 SD/JPEG/分页。
 */
#include "ebook.h"

#include "jpeg_codec.h"
#include "sd_card.h"
#include "ui_fonts.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "src/misc/lv_text_private.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "ebook";

#define EBOOK_DIR          SD_SYS_DIR "/BOOKS"
#define EBOOK_SD_LOCK_MS   5000U
#define EBOOK_WORKER_STACK 12288U
#define EBOOK_WORKER_PRIO  5
#define EBOOK_PAGE_TEXT_MAX 8192U

/* 封面源文件限制：解码输出按 16 对齐，最大 320x480（封面推荐 280x400）。
 * 更大或非 baseline 的封面只显示占位图，不影响 TXT 阅读。 */
#define EBOOK_COVER_JPEG_MAX (512U * 1024U)
#define EBOOK_COVER_DEC_W    320U
#define EBOOK_COVER_DEC_H    480U
#define EBOOK_COVER_DEC_BYTES (EBOOK_COVER_DEC_W * EBOOK_COVER_DEC_H * 2U)
/* 分页参数/字体排版变化时递增，使旧阅读进度自动失效。 */
#define EBOOK_PROGRESS_LAYOUT 1U

typedef struct {
    char title[EBOOK_TITLE_MAX]; /**< 文件夹名，也是书名 */
    char txt[EBOOK_TITLE_MAX];   /**< 文件夹内的 TXT 文件名 */
    char cover[EBOOK_TITLE_MAX]; /**< 文件夹内的封面文件名；可为空 */
    bool has_cover;
} ebook_book_t;

typedef struct {
    bool pending;
    uint32_t book;
    uint32_t token;  /**< 回调给 UI 的 token */
    uint32_t epoch;  /**< 内部 open/close 代次，用于中途取消 */
} ebook_open_req_t;

typedef struct {
    bool pending;
    uint32_t page;
    uint32_t token;
} ebook_page_req_t;

typedef struct {
    bool pending;
    uint32_t slot;
    uint32_t book;
    uint32_t token;
} ebook_cover_req_t;

typedef struct {
    bool pending;
    uint32_t book;
    uint32_t generation;
    uint32_t token;
} ebook_home_req_t;

static ebook_ops_t s_ops;
static void *s_ops_ctx;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_req_mux;
static SemaphoreHandle_t s_book_mux;

/* 书本表由 worker 构建；s_books 只在 s_book_mux 内交换/读取。 */
static ebook_book_t *s_books;
static ebook_book_t *s_scan_books;
static uint32_t s_count;
static bool s_truncated;
static uint32_t s_generation;

static atomic_bool s_scan_requested;
static atomic_uint s_open_epoch; /* open/close 代次；分页中途变化则放弃 */
static atomic_bool s_close_requested;

/* 请求全部由 s_req_mux 保护。 */
static ebook_open_req_t s_open_req;
static ebook_page_req_t s_page_req;
static ebook_cover_req_t s_cover_req[EBOOK_COVER_SLOTS];
static ebook_home_req_t s_home_req;

/* worker-only：当前打开的书、正文和页起始偏移。 */
static uint8_t *s_text;
static size_t s_text_len;
static uint32_t *s_pages;
static uint32_t s_page_count;
static uint32_t s_open_book = UINT32_MAX;
static uint64_t s_open_key; /* 当前书的阅读进度 key；关闭/失败清 0 */
static bool s_active;

/* 固定复用缓冲；首次真正需要时分配。 */
static uint8_t *s_cover_jpeg;
static uint8_t *s_cover_decode;
static uint8_t *s_cover_pixels[EBOOK_COVER_SLOTS];
static uint8_t *s_home_pixels;
static char *s_page_text;
static bool s_text_buffers_ok;
static bool s_cover_buffers_ok;

static void notify_worker(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* ============================ 书本快照 ============================ */

static bool book_snapshot(uint32_t index, ebook_book_t *out)
{
    if (!s_book_mux || !out || !s_books) return false;
    xSemaphoreTake(s_book_mux, portMAX_DELAY);
    bool ok = index < s_count;
    if (ok) *out = s_books[index];
    xSemaphoreGive(s_book_mux);
    return ok;
}

static bool book_path(const ebook_book_t *book, const char *name,
                      char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s/%s", EBOOK_DIR, book->title, name);
    return n > 0 && (size_t)n < cap;
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/**
 * @brief 阅读进度 key：书名 + TXT 大小 + 修改时间 + 排版版本。
 *
 * 同一本书换了 TXT 内容或改了分页参数后，旧页码不再可信，key 变化
 * 让 UI 从第一页开始，而不是跳到错误位置。返回 0 只用于失败路径。
 */
static uint64_t progress_key(const char *title, uint64_t size, uint32_t mtime,
                            int32_t line_height)
{
    uint64_t h = 1469598103934665603ULL;
    h = hash_bytes(h, title, strlen(title));
    uint8_t b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(size >> (i * 8));
    h = hash_bytes(h, b, 8);
    for (unsigned i = 0; i < 4; ++i) b[i] = (uint8_t)(mtime >> (i * 8));
    h = hash_bytes(h, b, 4);
    /* 分页参数和实际 18px 行高也进入 key：换了字号/字体度量或正文区域
     * 后，旧页码不能继续使用。EBOOK_PROGRESS_LAYOUT 用于分页算法本身。 */
    uint32_t layout[5] = {
        EBOOK_PROGRESS_LAYOUT, EBOOK_TEXT_W, EBOOK_TEXT_H,
        EBOOK_LINE_SPACE, (uint32_t)line_height,
    };
    for (unsigned i = 0; i < 5; ++i) {
        for (unsigned k = 0; k < 4; ++k)
            b[k] = (uint8_t)(layout[i] >> (k * 8));
        h = hash_bytes(h, b, 4);
    }
    return h ? h : 1U;
}

/* ============================ 扫描 ============================ */

static bool name_is_dir(const char *parent, const struct dirent *entry)
{
#ifndef _WIN32
    if (entry->d_type == DT_DIR) return true;
    if (entry->d_type != DT_UNKNOWN) return false;
#endif
    char path[sizeof(EBOOK_DIR) + EBOOK_TITLE_MAX + 2];
    int n = snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name);
    struct stat st;
    return n > 0 && (size_t)n < sizeof(path) &&
           stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool name_is_regular(const char *parent, const struct dirent *entry)
{
#ifndef _WIN32
    if (entry->d_type == DT_REG) return true;
    if (entry->d_type != DT_UNKNOWN) return false;
#endif
    char path[sizeof(EBOOK_DIR) + 2 * EBOOK_TITLE_MAX + 4];
    int n = snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name);
    struct stat st;
    return n > 0 && (size_t)n < sizeof(path) &&
           stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool txt_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".txt");
}

static bool cover_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static bool scan_book_folder(const char *title, ebook_book_t *book)
{
    char folder[sizeof(EBOOK_DIR) + EBOOK_TITLE_MAX + 2];
    int n = snprintf(folder, sizeof(folder), "%s/%s", EBOOK_DIR, title);
    if (n <= 0 || (size_t)n >= sizeof(folder)) return false;

    DIR *dir = opendir(folder);
    if (!dir) return false;

    bool has_txt = false;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        if (!name_is_regular(folder, e)) continue;
        if (!has_txt && txt_name(e->d_name)) {
            if (strlen(e->d_name) >= EBOOK_TITLE_MAX) continue;
            snprintf(book->txt, sizeof(book->txt), "%s", e->d_name);
            has_txt = true;
        } else if (!book->has_cover && cover_name(e->d_name)) {
            if (strlen(e->d_name) >= EBOOK_TITLE_MAX) continue;
            snprintf(book->cover, sizeof(book->cover), "%s", e->d_name);
            book->has_cover = true;
        }
    }
    closedir(dir);

    if (!has_txt) return false;
    snprintf(book->title, sizeof(book->title), "%s", title);
    return true;
}

static int book_compare(const void *a, const void *b)
{
    const ebook_book_t *ba = a;
    const ebook_book_t *bb = b;
    return strcasecmp(ba->title, bb->title);
}

static ebook_scan_result_t scan_books(uint32_t *out_count, bool *out_truncated,
                                      uint32_t *out_generation)
{
    *out_count = 0;
    *out_truncated = false;
    *out_generation = sd_card_get_generation();
    if (!sd_card_is_mounted() || !sd_card_is_present()) return EBOOK_SCAN_NO_CARD;
    if (!sd_card_io_lock(pdMS_TO_TICKS(EBOOK_SD_LOCK_MS))) return EBOOK_SCAN_BUSY;

    if (!s_scan_books) {
        s_scan_books = heap_caps_calloc(EBOOK_MAX_BOOKS, sizeof(ebook_book_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_scan_books) {
        sd_card_io_unlock();
        return EBOOK_SCAN_ERROR;
    }
    memset(s_scan_books, 0, EBOOK_MAX_BOOKS * sizeof(ebook_book_t));

    DIR *dir = opendir(EBOOK_DIR);
    ebook_scan_result_t result = EBOOK_SCAN_OK;
    uint32_t count = 0;
    bool truncated = false;
    if (!dir && errno != ENOENT) {
        int err = errno;
        sd_card_io_unlock();
        ESP_LOGW(TAG, "Cannot open %s: errno=%d", EBOOK_DIR, err);
        return EBOOK_SCAN_ERROR;
    }
    /* 目录不存在按空书库处理：仍要交换/清零旧表，避免 UI 清空后后台
     * 还保留上一轮的标题和路径。 */
    if (dir) {
        struct dirent *e;
        while (sd_card_is_present()) {
            errno = 0;
            e = readdir(dir);
            if (!e) {
                if (errno) {
                    ESP_LOGW(TAG, "Read BOOKS failed: errno=%d", errno);
                    result = EBOOK_SCAN_ERROR;
                }
                break;
            }
            if (e->d_name[0] == '.') continue;
            if (!name_is_dir(EBOOK_DIR, e)) continue;
            if (strlen(e->d_name) >= EBOOK_TITLE_MAX) {
                truncated = true;
                continue;
            }
            if (count >= EBOOK_MAX_BOOKS) {
                truncated = true;
                break;
            }
            ebook_book_t book;
            memset(&book, 0, sizeof(book));
            if (!scan_book_folder(e->d_name, &book)) continue;
            s_scan_books[count++] = book;
        }
        closedir(dir);
    }
    sd_card_io_unlock();

    if (result != EBOOK_SCAN_OK) return result;

    qsort(s_scan_books, count, sizeof(ebook_book_t), book_compare);

    /* 交换表指针：UI 只在 library 回调后读，仍用互斥保护标题访问。 */
    xSemaphoreTake(s_book_mux, portMAX_DELAY);
    ebook_book_t *old = s_books;
    s_books = s_scan_books;
    s_scan_books = old;
    s_count = count;
    s_truncated = truncated;
    s_generation = *out_generation;
    xSemaphoreGive(s_book_mux);

    *out_count = count;
    *out_truncated = truncated;
    ESP_LOGI(TAG, "Scan: %u books, truncated=%d", (unsigned)count, (int)truncated);
    return EBOOK_SCAN_OK;
}

static void publish_scan(ebook_scan_result_t result, uint32_t count,
                         bool truncated, uint32_t generation)
{
    if (!s_ops.library && !s_ops.scan_done) return;
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "LVGL busy; book list not published");
        return;
    }
    if (s_ops.library) s_ops.library(count, truncated, generation, s_ops_ctx);
    if (s_ops.scan_done) s_ops.scan_done(result, generation, s_ops_ctx);
    lvgl_port_unlock();
}

static void do_scan(void)
{
    atomic_store(&s_scan_requested, false);
    uint32_t count = 0, generation = sd_card_get_generation();
    bool truncated = false;
    ebook_scan_result_t result = scan_books(&count, &truncated, &generation);
    if (result != EBOOK_SCAN_OK) {
        xSemaphoreTake(s_book_mux, portMAX_DELAY);
        s_count = 0;
        s_truncated = false;
        s_generation = generation;
        xSemaphoreGive(s_book_mux);
        count = 0;
        truncated = false;
    }
    publish_scan(result, count, truncated, generation);
}

/* ============================ 封面 ============================ */

static bool ensure_cover_buffers(void)
{
    if (s_cover_buffers_ok) return true;
    s_cover_jpeg = heap_caps_malloc(EBOOK_COVER_JPEG_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* JPEG 硬件解码的输出缓冲必须满足 DMA/cache 对齐；必须使用
     * jpeg_alloc_decoder_mem，普通 heap_caps_malloc 会被驱动拒绝
     * （ESP_ERR_INVALID_ARG），封面就会全部显示占位图。 */
    jpeg_decode_memory_alloc_cfg_t out_mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual = 0;
    s_cover_decode = jpeg_alloc_decoder_mem(EBOOK_COVER_DEC_BYTES,
                                            &out_mem, &actual);
    s_home_pixels = heap_caps_malloc(EBOOK_HOME_W * EBOOK_HOME_H * 2U,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < EBOOK_COVER_SLOTS; ++i) {
        s_cover_pixels[i] = heap_caps_malloc(EBOOK_COVER_W * EBOOK_COVER_H * 2U,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    bool ok = s_cover_jpeg && s_cover_decode && s_home_pixels &&
              actual >= EBOOK_COVER_DEC_BYTES;
    for (unsigned i = 0; ok && i < EBOOK_COVER_SLOTS; ++i)
        ok = s_cover_pixels[i] != NULL;
    if (!ok) {
        ESP_LOGW(TAG, "Cover buffers unavailable");
        for (unsigned i = 0; i < EBOOK_COVER_SLOTS; ++i) {
            heap_caps_free(s_cover_pixels[i]);
            s_cover_pixels[i] = NULL;
        }
        heap_caps_free(s_cover_jpeg);
        heap_caps_free(s_cover_decode);
        heap_caps_free(s_home_pixels);
        s_cover_jpeg = NULL;
        s_cover_decode = NULL;
        s_home_pixels = NULL;
        return false;
    }
    s_cover_buffers_ok = true;
    return true;
}

/**
 * @brief 把源 RGB565 图缩放/裁剪到目标尺寸（box filter；放大用最近邻）。
 * @param src_stride 源图一行像素数（JPEG 输出 16 对齐后的 stride）。
 */
static void scale_rgb565(const uint8_t *src, unsigned sw, unsigned sh,
                         unsigned src_stride, uint8_t *dst,
                         unsigned dw, unsigned dh)
{
    const uint16_t *s = (const uint16_t *)src;
    uint16_t *d = (uint16_t *)dst;
    for (unsigned y = 0; y < dh; ++y) {
        unsigned y0 = y * sh / dh;
        unsigned y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (unsigned x = 0; x < dw; ++x) {
            unsigned x0 = x * sw / dw;
            unsigned x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                const uint16_t *row = s + (size_t)sy * src_stride;
                for (unsigned sx = x0; sx < x1; ++sx) {
                    uint16_t p = row[sx];
                    r += (p >> 11) & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    ++n;
                }
            }
            if (!n) n = 1;
            d[(size_t)y * dw + x] =
                (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

static bool read_cover(const ebook_book_t *book, uint32_t *bytes)
{
    *bytes = 0;
    if (!book->has_cover) {
        ESP_LOGW(TAG, "No cover file in book folder: %s", book->title);
        return false;
    }
    if (!ensure_cover_buffers()) return false;
    char path[sizeof(EBOOK_DIR) + 2 * EBOOK_TITLE_MAX + 4];
    if (!book_path(book, book->cover, path, sizeof(path))) return false;
    if (!sd_card_io_lock(pdMS_TO_TICKS(EBOOK_SD_LOCK_MS))) return false;

    struct stat st;
    bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              (uint64_t)st.st_size <= EBOOK_COVER_JPEG_MAX;
    if (ok) {
        FILE *fp = fopen(path, "rb");
        if (!fp) ok = false;
        else {
            size_t n = fread(s_cover_jpeg, 1, (size_t)st.st_size, fp);
            if (fclose(fp) != 0 || n != (size_t)st.st_size) ok = false;
            else *bytes = (uint32_t)n;
        }
    }
    sd_card_io_unlock();
    if (!ok) ESP_LOGW(TAG, "Cover read failed: %s", path);
    return ok;
}

/**
 * @brief 自己遍历 JPEG 标记找主图 SOF，跳过 APP0..APP15/EXIF 缩略图。
 *
 * IDF 的 jpeg_decoder_get_info() 只扫描第一个 FFC0；带 EXIF 缩略图的封面
 * 会被误报成缩略图尺寸（例如 120x120），随后主图解码尺寸对不上。这里按
 * 段长度跳过 APPn，只认主图 SOF0/SOF1；SOF2(progressive) 硬件不支持，
 * 直接返回 false 并让 UI 显示占位图。
 */
static bool jpeg_main_sof(const uint8_t *p, size_t n, unsigned *w,
                          unsigned *h, bool *baseline)
{
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) { ++i; continue; }
        while (i < n && p[i] == 0xFF) ++i;
        if (i >= n) break;
        uint8_t marker = p[i++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (i + 2 > n) break;
        uint16_t len = (uint16_t)((p[i] << 8) | p[i + 1]);
        if (len < 2 || i + len > n) break;
        if (marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (len < 7) return false;
            *h = (unsigned)((p[i + 3] << 8) | p[i + 4]);
            *w = (unsigned)((p[i + 5] << 8) | p[i + 6]);
            *baseline = (marker == 0xC0 || marker == 0xC1);
            return *w != 0 && *h != 0;
        }
        i += len;
    }
    return false;
}

static bool decode_cover(uint32_t bytes, unsigned *out_w, unsigned *out_h,
                         unsigned *stride)
{
    if (!ensure_cover_buffers() || !bytes) return false;
    unsigned w = 0, h = 0;
    bool baseline = false;
    if (!jpeg_main_sof(s_cover_jpeg, bytes, &w, &h, &baseline)) {
        ESP_LOGW(TAG, "Cover SOF missing");
        return false;
    }
    if (!baseline) {
        ESP_LOGW(TAG, "Cover is not baseline JPEG (progressive unsupported)");
        return false;
    }
    if (w > EBOOK_COVER_DEC_W || h > EBOOK_COVER_DEC_H) {
        ESP_LOGW(TAG, "Cover too large: %ux%u", w, h);
        return false;
    }
    unsigned pad_w = (w + 15U) & ~15U;
    unsigned pad_h = (h + 15U) & ~15U;
    if (pad_w > EBOOK_COVER_DEC_W || pad_h > EBOOK_COVER_DEC_H) return false;

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t out_size = 0;
    esp_err_t ret = jpeg_codec_process(&cfg, s_cover_jpeg, bytes,
                                       s_cover_decode, EBOOK_COVER_DEC_BYTES,
                                       &out_size, 1000);
    if (ret != ESP_OK || out_size != pad_w * pad_h * 2U) {
        ESP_LOGW(TAG, "Cover decode failed: %s (%u/%u, %ux%u)",
                 esp_err_to_name(ret), (unsigned)out_size,
                 (unsigned)(pad_w * pad_h * 2U), w, h);
        return false;
    }
    *out_w = w;
    *out_h = h;
    *stride = pad_w;
    return true;
}

static void do_cover(uint32_t slot, uint32_t book, uint32_t token)
{
    bool ok = false;
    if (slot < EBOOK_COVER_SLOTS && ensure_cover_buffers()) {
        ebook_book_t item;
        uint32_t bytes = 0;
        unsigned sw = 0, sh = 0, stride = 0;
        ok = book_snapshot(book, &item) &&
             read_cover(&item, &bytes) &&
             decode_cover(bytes, &sw, &sh, &stride);
        if (ok) {
            scale_rgb565(s_cover_decode, sw, sh, stride, s_cover_pixels[slot],
                         EBOOK_COVER_W, EBOOK_COVER_H);
        }
    }
    if (!s_ops.cover) return;
    if (lvgl_port_lock(1000)) {
        s_ops.cover(slot, book, ok ? s_cover_pixels[slot] : NULL,
                    EBOOK_COVER_W, EBOOK_COVER_H, token, ok, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void do_home_cover(uint32_t book, uint32_t generation, uint32_t token)
{
    bool ok = ensure_cover_buffers();
    if (ok) {
        ebook_book_t item;
        uint32_t bytes = 0;
        unsigned sw = 0, sh = 0, stride = 0;
        ok = book_snapshot(book, &item) && read_cover(&item, &bytes) &&
             decode_cover(bytes, &sw, &sh, &stride);
        if (ok) {
            scale_rgb565(s_cover_decode, sw, sh, stride, s_home_pixels,
                         EBOOK_HOME_W, EBOOK_HOME_H);
        }
    }
    if (!s_ops.home_cover) return;
    if (lvgl_port_lock(1000)) {
        s_ops.home_cover(book, ok ? s_home_pixels : NULL,
                         EBOOK_HOME_W, EBOOK_HOME_H, generation, ok,
                         s_ops_ctx);
        lvgl_port_unlock();
    }
}

/* ============================ TXT 打开/分页 ============================ */

static bool ensure_text_buffers(void)
{
    if (s_text_buffers_ok) return true;
    if (!s_page_text)
        s_page_text = heap_caps_malloc(EBOOK_PAGE_TEXT_MAX,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_text_buffers_ok = s_page_text != NULL;
    if (!s_text_buffers_ok) ESP_LOGW(TAG, "Page text buffer unavailable");
    return s_text_buffers_ok;
}

static void free_book(void)
{
    heap_caps_free(s_text);
    s_text = NULL;
    s_text_len = 0;
    heap_caps_free(s_pages);
    s_pages = NULL;
    s_page_count = 0;
    s_active = false;
    s_open_book = UINT32_MAX;
    s_open_key = 0;
}

/** @brief 严格 UTF-8 校验；拒绝过长编码、代理区和 >U+10FFFF。 */
static bool utf8_valid(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i++];
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned need;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need > n) return false;
        for (unsigned k = 0; k < need; ++k) {
            uint8_t cc = p[i++];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
    }
    return true;
}

/** @brief 把 CRLF/CR 统一成 LF（只在 UTF-8 校验通过后做，字节安全）。 */
static size_t normalize_newlines(uint8_t *p, size_t n)
{
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '\r') {
            p[out++] = '\n';
            if (i + 1 < n && p[i + 1] == '\n') ++i;
        } else {
            p[out++] = p[i];
        }
    }
    return out;
}

/**
 * @brief 按 18px 全字库和固定正文区域分页。
 *
 * 与 UI 的 label 使用同一字体/宽度/行距；页起始偏移存入 s_pages，之后
 * 取页只做 memcpy，不再触碰 SD。分页在 PSRAM 上进行，期间检查 open
 * 代次，UI 退出/换书会立即中止。
 */
static bool paginate_book(uint32_t epoch)
{
    const lv_font_t *font = ui_font_18();
    if (!font) return false;
    int32_t line_height = font->line_height + EBOOK_LINE_SPACE;
    if (line_height <= 0) return false;
    uint32_t lines_per_page = (uint32_t)EBOOK_TEXT_H / (uint32_t)line_height;
    if (lines_per_page == 0) lines_per_page = 1;

    if (!s_pages) {
        s_pages = heap_caps_malloc(EBOOK_MAX_PAGES * sizeof(uint32_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pages) return false;
    }

    lv_text_attributes_t attr = {0};
    attr.letter_space = 0;
    attr.line_space = EBOOK_LINE_SPACE;
    attr.max_width = EBOOK_TEXT_W;
    attr.text_flags = LV_TEXT_FLAG_NONE;

    uint32_t pos = 0;
    uint32_t pages = 0;
    while (pos < s_text_len) {
        if (atomic_load(&s_open_epoch) != epoch || atomic_load(&s_close_requested))
            return false;
        if (pages >= EBOOK_MAX_PAGES) return false;
        s_pages[pages++] = pos;
        for (uint32_t line = 0; line < lines_per_page && pos < s_text_len; ++line) {
            uint32_t adv = lv_text_get_next_line((const char *)s_text + pos,
                                                 (uint32_t)(s_text_len - pos),
                                                 font, NULL, &attr);
            if (adv == 0 || adv > s_text_len - pos) adv = 1;
            pos += adv;
        }
    }
    s_page_count = pages;
    return pages > 0;
}

static ebook_open_result_t load_book(uint32_t book, uint32_t epoch)
{
    ebook_book_t item;
    if (!book_snapshot(book, &item)) return EBOOK_OPEN_READ;
    if (!ensure_text_buffers()) return EBOOK_OPEN_TOO_LARGE;
    if (!sd_card_is_mounted() || !sd_card_is_present()) return EBOOK_OPEN_NO_CARD;

    char path[sizeof(EBOOK_DIR) + 2 * EBOOK_TITLE_MAX + 4];
    if (!book_path(&item, item.txt, path, sizeof(path))) return EBOOK_OPEN_READ;

    if (!sd_card_io_lock(pdMS_TO_TICKS(EBOOK_SD_LOCK_MS))) return EBOOK_OPEN_BUSY;
    struct stat st;
    bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    uint64_t size = ok ? (uint64_t)st.st_size : 0;
    uint32_t mtime = ok ? (uint32_t)st.st_mtime : 0;
    const lv_font_t *font = ui_font_18();
    uint64_t pkey = ok ? progress_key(item.title, size, mtime,
                                      font ? font->line_height : 0) : 0;
    if (ok && size > EBOOK_MAX_BYTES) {
        sd_card_io_unlock();
        return EBOOK_OPEN_TOO_LARGE;
    }
    if (ok && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + 1024U * 1024U) {
        sd_card_io_unlock();
        return EBOOK_OPEN_TOO_LARGE;
    }

    uint8_t *text = NULL;
    size_t bytes = 0;
    if (ok) {
        text = heap_caps_malloc((size_t)size + 1U,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!text) ok = false;
    }
    if (ok) {
        FILE *fp = fopen(path, "rb");
        if (!fp) ok = false;
        else {
            bytes = fread(text, 1, (size_t)size, fp);
            if (fclose(fp) != 0 || bytes != (size_t)size) ok = false;
        }
    }
    sd_card_io_unlock();
    if (!ok) {
        heap_caps_free(text);
        ESP_LOGW(TAG, "TXT read failed: %s", path);
        return EBOOK_OPEN_READ;
    }
    text[bytes] = '\0';

    size_t start = 0;
    if (bytes >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF)
        start = 3;
    if (!utf8_valid(text + start, bytes - start)) {
        heap_caps_free(text);
        ESP_LOGW(TAG, "TXT is not valid UTF-8: %s", path);
        return EBOOK_OPEN_UTF8;
    }
    size_t len = normalize_newlines(text + start, bytes - start);
    if (start) memmove(text, text + start, len);
    text[len] = '\0';

    /* 旧书先释放；从这里开始 s_text 归本次 open 所有。 */
    free_book();
    s_text = text;
    s_text_len = len;
    s_open_book = book;
    s_active = true;
    if (len == 0 || !paginate_book(epoch)) {
        free_book();
        ESP_LOGW(TAG, "TXT empty or pagination failed: %s", path);
        return len == 0 ? EBOOK_OPEN_READ : EBOOK_OPEN_TOO_MANY_PAGES;
    }
    s_open_key = pkey;
    ESP_LOGI(TAG, "Opened %s: %u bytes, %u pages", path, (unsigned)len,
             (unsigned)s_page_count);
    return EBOOK_OPEN_OK;
}

static void publish_opened(uint32_t book, uint32_t pages, uint64_t key,
                           uint32_t token, ebook_open_result_t result)
{
    if (!s_ops.opened) return;
    if (lvgl_port_lock(1000)) {
        s_ops.opened(book, pages, key, token, result, s_ops_ctx);
        lvgl_port_unlock();
    }
}

static void do_open(uint32_t book, uint32_t token, uint32_t epoch)
{
    /* 打开新书前先释放上一本的正文/页索引，避免两本大书同时占 PSRAM。
     * load_book 内部用 epoch 检查 UI 是否已经离开/换书。 */
    free_book();
    ebook_open_result_t result = load_book(book, epoch);
    if (atomic_load(&s_open_epoch) != epoch ||
        atomic_load(&s_close_requested)) {
        if (result == EBOOK_OPEN_OK) free_book();
        return;
    }
    if (result != EBOOK_OPEN_OK) free_book();
    publish_opened(book, result == EBOOK_OPEN_OK ? s_page_count : 0,
                   result == EBOOK_OPEN_OK ? s_open_key : 0, token, result);
}

static void do_page(uint32_t page, uint32_t token)
{
    if (!s_active || !s_text || !s_pages || page >= s_page_count) {
        if (s_ops.page && lvgl_port_lock(1000)) {
            s_ops.page(page, NULL, token, false, s_ops_ctx);
            lvgl_port_unlock();
        }
        return;
    }
    uint32_t start = s_pages[page];
    uint32_t end = (page + 1U < s_page_count) ? s_pages[page + 1U]
                                              : (uint32_t)s_text_len;
    if (end < start) end = start;
    size_t bytes = end - start;
    if (bytes >= EBOOK_PAGE_TEXT_MAX) bytes = EBOOK_PAGE_TEXT_MAX - 1U;
    memcpy(s_page_text, s_text + start, bytes);
    s_page_text[bytes] = '\0';

    if (s_ops.page && lvgl_port_lock(1000)) {
        s_ops.page(page, s_page_text, token, true, s_ops_ctx);
        lvgl_port_unlock();
    }
}

/* ============================ worker ============================ */

static void ebook_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (atomic_exchange(&s_scan_requested, false)) {
            do_scan();
            continue;
        }
        if (atomic_exchange(&s_close_requested, false)) {
            free_book();
            continue;
        }

        ebook_open_req_t open_req = {0};
        ebook_page_req_t page_req = {0};
        ebook_cover_req_t cover_req = {0};
        ebook_home_req_t home_req = {0};
        bool have_open = false, have_page = false, have_cover = false,
             have_home = false;
        xSemaphoreTake(s_req_mux, portMAX_DELAY);
        if (s_open_req.pending) {
            open_req = s_open_req;
            s_open_req.pending = false;
            have_open = true;
        } else if (s_page_req.pending) {
            page_req = s_page_req;
            s_page_req.pending = false;
            have_page = true;
        } else {
            for (unsigned i = 0; i < EBOOK_COVER_SLOTS; ++i) {
                if (s_cover_req[i].pending) {
                    cover_req = s_cover_req[i];
                    s_cover_req[i].pending = false;
                    have_cover = true;
                    break;
                }
            }
            if (!have_cover && s_home_req.pending) {
                home_req = s_home_req;
                s_home_req.pending = false;
                have_home = true;
            }
        }
        xSemaphoreGive(s_req_mux);

        if (have_open) do_open(open_req.book, open_req.token, open_req.epoch);
        else if (have_page) do_page(page_req.page, page_req.token);
        else if (have_cover) do_cover(cover_req.slot, cover_req.book, cover_req.token);
        else if (have_home)
            do_home_cover(home_req.book, home_req.generation, home_req.token);
        else
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

/* ============================ 公共接口 ============================ */

esp_err_t ebook_init(void)
{
    if (s_task) return ESP_OK;
    s_req_mux = xSemaphoreCreateMutex();
    s_book_mux = xSemaphoreCreateMutex();
    if (!s_req_mux || !s_book_mux) return ESP_ERR_NO_MEM;
    atomic_init(&s_scan_requested, false);
    atomic_init(&s_close_requested, false);
    atomic_init(&s_open_epoch, 0);
    if (xTaskCreatePinnedToCore(ebook_worker, "ebook", EBOOK_WORKER_STACK,
                                NULL, EBOOK_WORKER_PRIO, &s_task, 0) != pdPASS) {
        vSemaphoreDelete(s_req_mux);
        vSemaphoreDelete(s_book_mux);
        s_req_mux = s_book_mux = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ebook_set_ops(const ebook_ops_t *ops, void *ctx)
{
    s_ops = ops ? *ops : (ebook_ops_t){0};
    s_ops_ctx = ctx;
}

void ebook_request_scan(void)
{
    if (!s_task) return;
    atomic_store(&s_scan_requested, true);
    notify_worker();
}

uint32_t ebook_get_count(void)
{
    if (!s_book_mux) return 0;
    xSemaphoreTake(s_book_mux, portMAX_DELAY);
    uint32_t count = s_count;
    xSemaphoreGive(s_book_mux);
    return count;
}

bool ebook_has_cover(uint32_t index)
{
    if (!s_book_mux) return false;
    xSemaphoreTake(s_book_mux, portMAX_DELAY);
    bool has = s_books && index < s_count && s_books[index].has_cover;
    xSemaphoreGive(s_book_mux);
    return has;
}

bool ebook_get_title(uint32_t index, char *out, size_t cap)
{
    if (!s_book_mux || !out || !cap) return false;
    xSemaphoreTake(s_book_mux, portMAX_DELAY);
    bool ok = s_books && index < s_count;
    if (ok) snprintf(out, cap, "%s", s_books[index].title);
    xSemaphoreGive(s_book_mux);
    return ok;
}

void ebook_request_home_cover(uint32_t book, uint32_t generation, uint32_t token)
{
    if (!s_task || !s_req_mux) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    /* 只保留最新请求：换卡/重扫时旧封面请求的返回值会因 generation/token
     * 不匹配被 UI 丢弃，不需要排队解码旧书。 */
    s_home_req = (ebook_home_req_t){
        .pending = true, .book = book, .generation = generation, .token = token};
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void ebook_request_cover(uint32_t book, uint32_t slot, uint32_t token)
{
    if (!s_task || !s_req_mux || slot >= EBOOK_COVER_SLOTS) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_cover_req[slot] = (ebook_cover_req_t){
        .pending = true, .slot = slot, .book = book, .token = token};
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void ebook_open(uint32_t book, uint32_t token)
{
    if (!s_task || !s_req_mux) return;
    uint32_t epoch = atomic_fetch_add(&s_open_epoch, 1) + 1;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_open_req = (ebook_open_req_t){.pending = true, .book = book,
                                    .token = token, .epoch = epoch};
    s_page_req.pending = false;
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void ebook_request_page(uint32_t page, uint32_t token)
{
    if (!s_task || !s_req_mux) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_page_req = (ebook_page_req_t){.pending = true, .page = page, .token = token};
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void ebook_close(void)
{
    if (!s_task || !s_req_mux) return;
    atomic_fetch_add(&s_open_epoch, 1);
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_open_req.pending = false;
    s_page_req.pending = false;
    xSemaphoreGive(s_req_mux);
    atomic_store(&s_close_requested, true);
    notify_worker();
}
