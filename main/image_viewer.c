/**
 * @file image_viewer.c
 * @brief 图片查看器后台 worker：目录扫描、固定缓冲 JPEG 解码和 UI 发布。
 *
 * 内存策略（见 image_viewer.h）：
 * - 两个文件夹各一份固定文件名表，扫描时只写名字，不读文件内容。
 * - JPEG 输入、解码 scratch、两块全屏帧、32 个缩略图槽和两个首页预览
 *   在首次需要时一次性分配并复用；任何单张图片、滚动速度或图片数量都
 *   不会触发新的图片缓冲分配。
 * - 扫描/解码失败只发布 ok=false，不清空已经显示的其他缓冲，也不伪造
 *   图片或文件名。
 */
#include "image_viewer.h"

#include "jpeg_codec.h"
#include "sd_card.h"
#include "thumb_cache.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "image_viewer";

#define IMAGE_DIR_CAM       SD_SYS_DIR "/MEDIA/cam"
#define IMAGE_DIR_PICTURES  SD_SYS_DIR "/MEDIA/pictures"
#define IMAGE_SD_LOCK_MS    5000U
/* 12 KiB：与音乐扫描 worker 相当；JPEG 输出/输入缓冲都在 PSRAM 堆上，
 * 任务栈只保留 FatFs 调用和局部路径，避免挤占内部 DMA 余量。 */
#define IMAGE_WORKER_STACK  12288U
#define IMAGE_WORKER_PRIO   5

static const char *const s_dirs[IMAGE_FOLDER_COUNT] = {
    IMAGE_DIR_CAM,
    IMAGE_DIR_PICTURES,
};

typedef struct {
    bool pending;
    image_folder_t folder;
    uint32_t index;
    uint32_t token;
} thumb_request_t;

/* 全屏预取：两块独立的全屏缓冲，分别缓存上一张/下一张。它们只由
 * worker 读写；UI 通过 s_full_active 原子标志开/关。 */
#define IMAGE_PREFETCH_SLOTS 2
typedef struct {
    bool valid;      /**< pixels 中已有对应图片 */
    bool pending;    /**< 已排队，等待 worker 解码 */
    image_folder_t folder;
    uint32_t index;
    uint32_t generation;
    uint8_t *pixels;
} prefetch_slot_t;

static image_viewer_ops_t s_ops;
static void *s_ops_ctx;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_req_mux;

/* worker-only 文件名/计数；UI 只通过 LVGL 回调取得快照。 */
static char *s_names[IMAGE_FOLDER_COUNT];
static uint32_t s_count[IMAGE_FOLDER_COUNT];
static bool s_truncated[IMAGE_FOLDER_COUNT];
static bool s_ready[IMAGE_FOLDER_COUNT];
static uint32_t s_generation[IMAGE_FOLDER_COUNT];

/* 固定 PSRAM 缓冲，首次扫描时一起分配。 */
static uint8_t *s_jpeg_buf;
static uint8_t *s_scratch;
static uint8_t *s_full_pixels[2];
static uint8_t *s_thumb_pixels[IMAGE_MAX_THUMB_SLOTS];
static uint8_t *s_home_pixels[IMAGE_FOLDER_COUNT];
static bool s_buffers_ok;

/* 每个缩略图槽位最近一次成功解码的身份。总览关闭时 UI 会失效槽位，
 * 但后台缓冲仍然保留；重新打开同一窗口时直接重发这张已解码的小图，
 * 不访问 SD、不再解码。扫描/换卡才清空。 */
typedef struct {
    bool valid;
    image_folder_t folder;
    uint32_t index;
    uint32_t generation;
} thumb_mem_t;
static thumb_mem_t s_thumb_mem[IMAGE_MAX_THUMB_SLOTS];
static uint8_t s_full_next;
/* worker-only：最近一次读/解码失败的具体原因，供全屏回调显示。 */
static image_error_t s_last_error;
static prefetch_slot_t s_prefetch[IMAGE_PREFETCH_SLOTS];
static bool s_prefetch_center_valid;
static image_folder_t s_prefetch_center_folder;
static uint32_t s_prefetch_center_index;
static uint32_t s_prefetch_center_generation;
static atomic_bool s_full_active; /* UI 全屏状态：控制预取开关 */

/* 请求状态由 s_req_mux 保护；扫描标志是无锁原子。 */
static atomic_bool s_scan_requested;
static bool s_full_pending;
static image_folder_t s_full_folder;
static uint32_t s_full_index;
static uint32_t s_full_token;
static uint32_t s_full_epoch;
static thumb_request_t s_thumb_req[IMAGE_MAX_THUMB_SLOTS];
static uint32_t s_thumb_clear_epoch;
/* 连续处理满一屏后打印一次命中率，便于确认缓存生效；只做计数。 */
static unsigned s_thumb_total;
static unsigned s_thumb_hits;

static void notify_worker(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* ============================ 固定缓冲 ============================ */

static void free_buffers(void)
{
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        heap_caps_free(s_names[f]);
        s_names[f] = NULL;
        heap_caps_free(s_home_pixels[f]);
        s_home_pixels[f] = NULL;
    }
    heap_caps_free(s_jpeg_buf);
    s_jpeg_buf = NULL;
    heap_caps_free(s_scratch);
    s_scratch = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        heap_caps_free(s_full_pixels[i]);
        s_full_pixels[i] = NULL;
    }
    for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
        heap_caps_free(s_prefetch[i].pixels);
        s_prefetch[i].pixels = NULL;
        s_prefetch[i].valid = false;
        s_prefetch[i].pending = false;
    }
    s_prefetch_center_valid = false;
    for (unsigned i = 0; i < IMAGE_MAX_THUMB_SLOTS; ++i) {
        heap_caps_free(s_thumb_pixels[i]);
        s_thumb_pixels[i] = NULL;
        s_thumb_mem[i].valid = false;
    }
    s_buffers_ok = false;
}

/**
 * @brief 一次性分配所有图片缓冲；失败时全部释放，下一次请求再重试。
 *
 * 这些缓冲与图片数量无关，随后只复用。输出缓冲通过 jpeg_alloc_decoder_mem
 * 分配，满足 JPEG 解码器对 cache/DMA 对齐的要求。
 */
static bool ensure_buffers(void)
{
    if (s_buffers_ok) return true;

    const size_t names_bytes = (size_t)IMAGE_INDEX_MAX * IMAGE_NAME_MAX;
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f)
        s_names[f] = heap_caps_calloc(1, names_bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_jpeg_buf = heap_caps_malloc(IMAGE_JPEG_MAX_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    jpeg_decode_memory_alloc_cfg_t out_mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual = 0;
    s_scratch = jpeg_alloc_decoder_mem(IMAGE_FULL_BYTES, &out_mem, &actual);
    bool ok = s_scratch && actual >= IMAGE_FULL_BYTES;
    for (unsigned i = 0; ok && i < 2; ++i) {
        s_full_pixels[i] = jpeg_alloc_decoder_mem(IMAGE_FULL_BYTES, &out_mem, &actual);
        ok = s_full_pixels[i] && actual >= IMAGE_FULL_BYTES;
    }
    for (unsigned i = 0; ok && i < IMAGE_PREFETCH_SLOTS; ++i) {
        s_prefetch[i].pixels = jpeg_alloc_decoder_mem(IMAGE_FULL_BYTES, &out_mem, &actual);
        ok = s_prefetch[i].pixels && actual >= IMAGE_FULL_BYTES;
        s_prefetch[i].valid = false;
        s_prefetch[i].pending = false;
    }
    for (unsigned i = 0; ok && i < IMAGE_MAX_THUMB_SLOTS; ++i) {
        s_thumb_pixels[i] = heap_caps_malloc(IMAGE_THUMB_W * IMAGE_THUMB_H * 2U,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = s_thumb_pixels[i] != NULL;
    }
    for (unsigned f = 0; ok && f < IMAGE_FOLDER_COUNT; ++f) {
        s_home_pixels[f] = heap_caps_malloc(IMAGE_HOME_W * IMAGE_HOME_H * 2U,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = s_home_pixels[f] != NULL;
    }
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f)
        ok = ok && s_names[f] != NULL;
    ok = ok && s_jpeg_buf != NULL;

    if (!ok) {
        ESP_LOGW(TAG, "Image buffers unavailable; fix PSRAM pressure before retrying");
        free_buffers();
        return false;
    }
    s_buffers_ok = true;
    ESP_LOGI(TAG, "Image buffers ready: names=%u B, jpeg=%u B, thumbs=%u x %u B",
             (unsigned)(names_bytes * IMAGE_FOLDER_COUNT),
             (unsigned)IMAGE_JPEG_MAX_BYTES,
             (unsigned)IMAGE_MAX_THUMB_SLOTS,
             (unsigned)(IMAGE_THUMB_W * IMAGE_THUMB_H * 2U));
    return true;
}

/* ============================ 扫描 ============================ */

static bool image_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static int name_compare(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/**
 * @brief 扫描一个文件夹到固定名字表。
 *
 * 整个目录遍历期间持有项目 SD I/O 锁，保证 DIR 句柄不会在遍历中被
 * VFS 卸载/重挂载。目录不存在视为空目录；SD 未挂载/拔卡/读目录失败
 * 返回错误，并清空计数。
 */
static image_scan_result_t scan_folder(image_folder_t folder, uint32_t gen)
{
    s_count[folder] = 0;
    s_truncated[folder] = false;
    s_ready[folder] = false;
    s_generation[folder] = gen;

    ESP_LOGI(TAG, "Scan %s: gen=%u", s_dirs[folder], (unsigned)gen);
    if (!sd_card_is_mounted() || !sd_card_is_present()) {
        ESP_LOGW(TAG, "Scan %s: SD not mounted/present", s_dirs[folder]);
        return IMAGE_SCAN_NO_CARD;
    }
    if (!sd_card_io_lock(pdMS_TO_TICKS(IMAGE_SD_LOCK_MS))) {
        ESP_LOGW(TAG, "Scan %s: SD I/O lock timeout", s_dirs[folder]);
        return IMAGE_SCAN_BUSY;
    }

    DIR *dir = opendir(s_dirs[folder]);
    if (!dir) {
        int err = errno;
        sd_card_io_unlock();
        if (err == ENOENT) {
            s_ready[folder] = true; /* 空目录，不是 I/O 错误 */
            ESP_LOGI(TAG, "Scan %s: directory missing, treated as empty",
                     s_dirs[folder]);
            return IMAGE_SCAN_OK;
        }
        ESP_LOGW(TAG, "Cannot open %s: errno=%d", s_dirs[folder], err);
        return IMAGE_SCAN_ERROR;
    }

    uint32_t count = 0;
    bool truncated = false;
    image_scan_result_t result = IMAGE_SCAN_OK;
    while (true) {
        if (!sd_card_is_present()) {
            result = IMAGE_SCAN_NO_CARD;
            break;
        }
        if (!sd_card_is_mounted() || sd_card_get_generation() != gen) {
            result = IMAGE_SCAN_BUSY;
            break;
        }
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) {
            if (errno) {
                ESP_LOGW(TAG, "Read %s failed: errno=%d", s_dirs[folder], errno);
                result = IMAGE_SCAN_ERROR;
            }
            break;
        }
        if (e->d_name[0] == '.' || !image_ext(e->d_name)) continue;

        bool regular = e->d_type == DT_REG;
        if (e->d_type == DT_UNKNOWN) {
            char path[sizeof(IMAGE_DIR_PICTURES) + IMAGE_NAME_MAX + 4];
            int n = snprintf(path, sizeof(path), "%s/%s", s_dirs[folder], e->d_name);
            struct stat st;
            regular = n > 0 && (size_t)n < sizeof(path) &&
                      stat(path, &st) == 0 && S_ISREG(st.st_mode);
        }
        if (!regular) continue;

        if (strlen(e->d_name) >= IMAGE_NAME_MAX) {
            truncated = true;
            continue;
        }
        if (count >= IMAGE_INDEX_MAX) {
            truncated = true;
            break;
        }
        memcpy(s_names[folder] + (size_t)count * IMAGE_NAME_MAX, e->d_name,
               strlen(e->d_name) + 1);
        ++count;
    }

    closedir(dir);
    sd_card_io_unlock();

    if (result != IMAGE_SCAN_OK) {
        count = 0;
        truncated = false;
    } else if (count > 1) {
        /* ready 仍为 false，UI 不会在排序期间读取名字表。 */
        qsort(s_names[folder], count, IMAGE_NAME_MAX, name_compare);
    }
    s_count[folder] = count;
    s_truncated[folder] = truncated;
    s_ready[folder] = result == IMAGE_SCAN_OK;
    ESP_LOGI(TAG, "Scan %s: %u images, truncated=%d, result=%d",
             s_dirs[folder], (unsigned)count, (int)truncated, (int)result);
    return result;
}

static bool build_path(image_folder_t folder, uint32_t index, char *out, size_t cap)
{
    if (folder >= IMAGE_FOLDER_COUNT || !s_ready[folder] ||
        index >= s_count[folder]) return false;
    int n = snprintf(out, cap, "%s/%s", s_dirs[folder],
                     s_names[folder] + (size_t)index * IMAGE_NAME_MAX);
    return n > 0 && (size_t)n < cap;
}

/**
 * @brief 读取一张 JPEG 到固定输入缓冲；不进行任何按文件大小分配。
 * @return 读取到的字节数，失败返回 0；s_last_error 记录原因。
 */
static size_t read_jpeg(image_folder_t folder, uint32_t index)
{
    s_last_error = IMAGE_ERROR_NONE;
    char path[sizeof(IMAGE_DIR_PICTURES) + IMAGE_NAME_MAX + 4];
    if (!build_path(folder, index, path, sizeof(path))) {
        s_last_error = IMAGE_ERROR_READ;
        return 0;
    }
    if (!sd_card_io_lock(pdMS_TO_TICKS(IMAGE_SD_LOCK_MS))) {
        s_last_error = IMAGE_ERROR_BUSY;
        ESP_LOGW(TAG, "Read %s: SD I/O lock timeout", path);
        return 0;
    }
    size_t bytes = 0;
    struct stat st;
    if (!sd_card_is_mounted() || !sd_card_is_present() ||
        sd_card_get_generation() != s_generation[folder]) {
        s_last_error = IMAGE_ERROR_BUSY;
        ESP_LOGW(TAG, "Read %s: SD state changed (mounted=%d present=%d gen=%u/%u)",
                 path, (int)sd_card_is_mounted(), (int)sd_card_is_present(),
                 (unsigned)sd_card_get_generation(),
                 (unsigned)s_generation[folder]);
    } else if (stat(path, &st) != 0) {
        s_last_error = IMAGE_ERROR_READ;
        ESP_LOGW(TAG, "Read %s: stat failed errno=%d", path, errno);
    } else if (!S_ISREG(st.st_mode)) {
        s_last_error = IMAGE_ERROR_READ;
        ESP_LOGW(TAG, "Read %s: not a regular file", path);
    } else if (st.st_size <= 0) {
        s_last_error = IMAGE_ERROR_READ;
        ESP_LOGW(TAG, "Read %s: empty file", path);
    } else if ((uint64_t)st.st_size > IMAGE_JPEG_MAX_BYTES) {
        s_last_error = IMAGE_ERROR_READ;
        ESP_LOGW(TAG, "Read %s: file too large (%lld > %u)",
                 path, (long long)st.st_size, (unsigned)IMAGE_JPEG_MAX_BYTES);
    } else {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            s_last_error = IMAGE_ERROR_READ;
            ESP_LOGW(TAG, "Read %s: fopen failed errno=%d", path, errno);
        } else {
            bytes = fread(s_jpeg_buf, 1, (size_t)st.st_size, fp);
            fclose(fp);
            if (bytes != (size_t)st.st_size) {
                ESP_LOGW(TAG, "Read %s: short read %u/%lld", path,
                         (unsigned)bytes, (long long)st.st_size);
                s_last_error = IMAGE_ERROR_READ;
                bytes = 0;
            }
        }
    }
    sd_card_io_unlock();
    return bytes;
}

/* ============================ 解码/缩放 ============================ */

/**
 * @brief 把输入缓冲解码到指定 464x464 RGB565 输出缓冲。
 *
 * 不再先用 jpeg_decoder_get_info() 预检查尺寸：IDF 这个辅助函数只扫描
 * 第一个 FFC0，不会跳过 APP1/EXIF，带内嵌缩略图的 460x460 主图会被误判
 * 成缩略图尺寸。这里直接交给硬件解码器，它的 jpeg_parse_marker() 会按
 * 长度跳过 APP0..APP15，再解析主图 SOF0；输出尺寸/格式由解码结果判断。
 *
 * - 输出缓冲只有 464x464，主图大于该尺寸时驱动返回 ESP_ERR_INVALID_ARG；
 * - progressive/非 baseline JPEG 返回 ESP_ERR_NOT_SUPPORTED；
 * - 尺寸小于 460 的图会成功解码但 out_size != 464x464，仍按尺寸不符处理。
 * 这样既保留 460x460 要求，又不会把 EXIF 缩略图当成主图。
 */
static bool decode_full(size_t bytes, uint8_t *out)
{
    s_last_error = IMAGE_ERROR_NONE;
    if (!bytes || !out) {
        s_last_error = IMAGE_ERROR_READ;
        return false;
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* 与锁屏/视频一致：BGR 选择 LVGL 需要的原生小端 RGB565。 */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t out_size = 0;
    esp_err_t ret = jpeg_codec_process(&cfg, s_jpeg_buf, (uint32_t)bytes,
                                       out, IMAGE_FULL_BYTES, &out_size, 1000);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_ARG) {
            s_last_error = IMAGE_ERROR_SIZE; /* 主图输出大于464x464缓冲 */
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            s_last_error = IMAGE_ERROR_FORMAT; /* progressive/非baseline */
        } else {
            s_last_error = IMAGE_ERROR_FORMAT;
        }
        ESP_LOGW(TAG, "JPEG decode failed: %s (%u bytes, out=%u/%u)",
                 esp_err_to_name(ret), (unsigned)bytes, (unsigned)out_size,
                 (unsigned)IMAGE_FULL_BYTES);
        return false;
    }
    if (out_size != IMAGE_FULL_BYTES) {
        s_last_error = IMAGE_ERROR_SIZE;
        ESP_LOGW(TAG, "JPEG decoded output %u != %u (need a 460x460 main image)",
                 (unsigned)out_size, (unsigned)IMAGE_FULL_BYTES);
        return false;
    }
    return true;
}

/**
 * @brief 把 464 行 stride 的 460x460 RGB565 图缩成 dw x dh。
 *
 * 源图 stride 是 JPEG 输出按 16 对齐后的 464；只采样有效 460x460 区域。
 * box filter 的中间累加使用 32 位，100/148 目标尺寸下不会溢出。
 */
static void downscale_460(const uint8_t *src, uint8_t *dst,
                          unsigned dw, unsigned dh)
{
    const uint16_t *s = (const uint16_t *)src;
    uint16_t *d = (uint16_t *)dst;
    const unsigned sw = IMAGE_FULL_W;
    const unsigned sh = IMAGE_FULL_H;
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
                const uint16_t *row = s + (size_t)sy * IMAGE_FULL_PAD_W;
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

static bool decode_to_scratch(size_t bytes)
{
    return decode_full(bytes, s_scratch);
}

/* ============================ UI 发布 ============================ */

static bool ops_available(void)
{
    return s_ops.library || s_ops.home_preview || s_ops.scan_done ||
           s_ops.thumbnail || s_ops.full_image;
}

static void publish_scan(image_scan_result_t result, uint32_t gen,
                         const bool home_ok[IMAGE_FOLDER_COUNT])
{
    if (!ops_available()) return;
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "LVGL busy; scan snapshot not published");
        return;
    }
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        if (s_ops.library)
            s_ops.library((image_folder_t)f, s_count[f], s_truncated[f],
                          gen, s_ops_ctx);
    }
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        if (s_ops.home_preview)
            s_ops.home_preview((image_folder_t)f,
                               home_ok[f] ? s_home_pixels[f] : NULL,
                               IMAGE_HOME_W, IMAGE_HOME_H, gen, home_ok[f],
                               s_ops_ctx);
    }
    if (s_ops.scan_done) s_ops.scan_done(result, gen, s_ops_ctx);
    lvgl_port_unlock();
}

static void publish_thumbnail(uint32_t slot, uint32_t token, bool ok)
{
    if (!s_ops.thumbnail) return;
    if (!lvgl_port_lock(100)) return;
    s_ops.thumbnail(slot, ok ? s_thumb_pixels[slot] : NULL,
                    IMAGE_THUMB_W, IMAGE_THUMB_H, token, ok, s_ops_ctx);
    lvgl_port_unlock();
}

static void publish_full(image_folder_t folder, uint32_t index,
                         const char *name, uint32_t token, bool ok,
                         image_error_t error, const uint8_t *pixels)
{
    if (!s_ops.full_image) return;
    if (!lvgl_port_lock(1000)) return;
    s_ops.full_image(pixels, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H,
                     folder, index, name, token, ok, error, s_ops_ctx);
    lvgl_port_unlock();
}

/* ============================ 全屏相邻预取 ============================ */

/** @brief 清空预取槽与中心；只在 worker 中调用。 */
static void prefetch_clear(void)
{
    for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
        s_prefetch[i].valid = false;
        s_prefetch[i].pending = false;
    }
    s_prefetch_center_valid = false;
}

static bool prefetch_slot_matches(const prefetch_slot_t *slot,
                                  image_folder_t folder, uint32_t index,
                                  uint32_t generation)
{
    return (slot->valid || slot->pending) && slot->folder == folder &&
           slot->index == index && slot->generation == generation;
}

/**
 * @brief 按当前全屏图片规划上一张/下一张预取。
 *
 * 保留仍然相邻的缓存，丢弃离开窗口的槽；下一张优先占用第一个空闲槽，
 * 这样用户最常做的“左滑下一张”最先就绪。整个函数只操作 worker 状态。
 */
static void prefetch_plan(void)
{
    if (!atomic_load(&s_full_active) || !s_prefetch_center_valid) {
        prefetch_clear();
        return;
    }
    image_folder_t folder = s_prefetch_center_folder;
    if (folder >= IMAGE_FOLDER_COUNT || !s_ready[folder]) {
        prefetch_clear();
        return;
    }
    uint32_t count = s_count[folder];
    uint32_t generation = s_generation[folder];
    if (count <= 1 || s_prefetch_center_index >= count ||
        generation != s_prefetch_center_generation) {
        prefetch_clear();
        return;
    }

    uint32_t targets[IMAGE_PREFETCH_SLOTS];
    unsigned target_count = 0;
    uint32_t next = (s_prefetch_center_index + 1U) % count;
    uint32_t prev = (s_prefetch_center_index + count - 1U) % count;
    targets[target_count++] = next;
    if (prev != next) targets[target_count++] = prev;

    for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
        prefetch_slot_t *slot = &s_prefetch[i];
        if (!slot->valid && !slot->pending) continue;
        bool keep = false;
        for (unsigned t = 0; t < target_count; ++t) {
            if (slot->folder == folder && slot->index == targets[t] &&
                slot->generation == generation) {
                keep = true;
                break;
            }
        }
        if (!keep) {
            slot->valid = false;
            slot->pending = false;
        }
    }

    for (unsigned t = 0; t < target_count; ++t) {
        bool have = false;
        for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
            if (prefetch_slot_matches(&s_prefetch[i], folder, targets[t],
                                      generation)) {
                have = true;
                break;
            }
        }
        if (have) continue;
        for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
            prefetch_slot_t *slot = &s_prefetch[i];
            if (slot->valid || slot->pending) continue;
            slot->folder = folder;
            slot->index = targets[t];
            slot->generation = generation;
            slot->valid = false;
            slot->pending = true;
            break;
        }
    }
}

/** @brief 如果请求图片已预取，复制到显示缓冲并消耗缓存（比重新解码快得多）。 */
static bool prefetch_take(image_folder_t folder, uint32_t index, uint8_t *out)
{
    uint32_t generation = folder < IMAGE_FOLDER_COUNT ?
                          s_generation[folder] : 0;
    for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
        prefetch_slot_t *slot = &s_prefetch[i];
        if (slot->valid && slot->folder == folder && slot->index == index &&
            slot->generation == generation) {
            memcpy(out, slot->pixels, IMAGE_FULL_BYTES);
            slot->valid = false;
            slot->pending = false;
            return true;
        }
    }
    return false;
}

/** @brief 解码一个已排队的预取槽；返回 true 表示本轮做了工作。 */
static bool process_prefetch(void)
{
    if (!atomic_load(&s_full_active)) {
        prefetch_clear();
        return false;
    }
    if (!s_prefetch_center_valid) return false;
    for (unsigned i = 0; i < IMAGE_PREFETCH_SLOTS; ++i) {
        prefetch_slot_t *slot = &s_prefetch[i];
        if (!slot->pending) continue;
        slot->pending = false;
        if (slot->folder >= IMAGE_FOLDER_COUNT || !s_ready[slot->folder] ||
            slot->index >= s_count[slot->folder] ||
            slot->generation != s_generation[slot->folder]) {
            slot->valid = false;
            return true;
        }
        size_t bytes = read_jpeg(slot->folder, slot->index);
        bool ok = bytes && decode_full(bytes, slot->pixels);
        if (!atomic_load(&s_full_active)) {
            slot->valid = false;
            return true;
        }
        slot->valid = ok;
        ESP_LOGI(TAG, "Prefetch %s #%u %s", s_dirs[slot->folder],
                 (unsigned)slot->index, ok ? "ready" : "failed");
        return true;
    }
    return false;
}

/* ============================ worker 工作项 ============================ */

static void do_scan(void)
{
    atomic_store(&s_scan_requested, false);
    uint32_t gen = sd_card_get_generation();
    bool home_ok[IMAGE_FOLDER_COUNT] = {false, false};
    /* 目录可能变化：上一轮的缩略图身份全部作废，避免重扫后误用旧小图。 */
    for (unsigned i = 0; i < IMAGE_MAX_THUMB_SLOTS; ++i)
        s_thumb_mem[i].valid = false;

    if (!ensure_buffers()) {
        for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
            s_count[f] = 0;
            s_truncated[f] = false;
            s_ready[f] = false;
            s_generation[f] = gen;
        }
        publish_scan(IMAGE_SCAN_ERROR, gen, home_ok);
        return;
    }

    image_scan_result_t result = scan_folder(IMAGE_FOLDER_CAM, gen);
    if (result == IMAGE_SCAN_OK) {
        image_scan_result_t second = scan_folder(IMAGE_FOLDER_PICTURES, gen);
        if (second != IMAGE_SCAN_OK) result = second;
    } else {
        /* 第一轮失败时不要再对同一张可能已拔出的卡做第二次目录遍历。 */
        s_count[IMAGE_FOLDER_PICTURES] = 0;
        s_truncated[IMAGE_FOLDER_PICTURES] = false;
        s_ready[IMAGE_FOLDER_PICTURES] = false;
        s_generation[IMAGE_FOLDER_PICTURES] = gen;
    }

    if (result == IMAGE_SCAN_OK) {
        for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
            if (!s_ready[f] || !s_count[f]) continue;
            char path[sizeof(IMAGE_DIR_PICTURES) + IMAGE_NAME_MAX + 4];
            bool path_ok = build_path((image_folder_t)f, 0, path, sizeof(path));
            /* 首页预览也走缩略图缓存：避免每次扫描都解码两张 460x460
             * 原图。源文件没变时直接复用上次缩小后的 148x148 小图。 */
            if (path_ok &&
                thumb_cache_load(THUMB_CACHE_HOME, path, IMAGE_HOME_W,
                                 IMAGE_HOME_H, s_home_pixels[f])) {
                home_ok[f] = true;
                continue;
            }
            size_t bytes = read_jpeg((image_folder_t)f, 0);
            if (bytes && decode_to_scratch(bytes)) {
                downscale_460(s_scratch, s_home_pixels[f],
                              IMAGE_HOME_W, IMAGE_HOME_H);
                home_ok[f] = true;
                if (path_ok)
                    thumb_cache_store(THUMB_CACHE_HOME, path, IMAGE_HOME_W,
                                      IMAGE_HOME_H, s_home_pixels[f]);
            }
        }
    }
    ESP_LOGI(TAG, "Scan finished: result=%d cam=%u pictures=%u",
             (int)result, (unsigned)s_count[IMAGE_FOLDER_CAM],
             (unsigned)s_count[IMAGE_FOLDER_PICTURES]);
    publish_scan(result, gen, home_ok);
}

/** @brief 处理一次全屏请求；返回 true 表示本轮做了工作。 */
static bool process_full(void)
{
    if (!s_req_mux) return false;
    image_folder_t folder;
    uint32_t index, token, epoch;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    if (!s_full_pending) {
        xSemaphoreGive(s_req_mux);
        return false;
    }
    folder = s_full_folder;
    index = s_full_index;
    token = s_full_token;
    epoch = s_full_epoch;
    s_full_pending = false;
    xSemaphoreGive(s_req_mux);

    bool ok = false;
    image_error_t error = IMAGE_ERROR_NONE;
    char name[IMAGE_NAME_MAX] = {0};
    uint8_t *out = NULL;
    if (!ensure_buffers()) {
        error = IMAGE_ERROR_READ;
    } else if (folder >= IMAGE_FOLDER_COUNT || !s_ready[folder] ||
               index >= s_count[folder]) {
        error = IMAGE_ERROR_READ;
    } else {
        out = s_full_pixels[s_full_next];
        snprintf(name, sizeof(name), "%s",
                 s_names[folder] + (size_t)index * IMAGE_NAME_MAX);
        if (prefetch_take(folder, index, out)) {
            /* 预取命中：430KB 复制远快于重新读 SD + JPEG 解码。 */
            ok = true;
        } else {
            size_t bytes = read_jpeg(folder, index);
            if (!bytes) {
                error = s_last_error;
            } else if (!decode_full(bytes, out)) {
                error = s_last_error;
            } else {
                ok = true;
            }
        }
    }

    /* 解码期间 UI 可能取消或切到下一张：epoch 变了就丢弃这次结果。 */
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    bool valid = epoch == s_full_epoch;
    xSemaphoreGive(s_req_mux);
    if (valid) {
        /* 只有真正发布出去的结果才切换后备缓冲；被取消/替换的迟到解码
         * 仍写在同一块后备缓冲里，绝不会覆盖 UI 正在显示的另一块。 */
        if (ok) s_full_next ^= 1;
        publish_full(folder, index, name, token, ok, error,
                     ok ? out : NULL);
        if (ok) {
            /* 当前图片已经发布；更新预取中心并规划上一张/下一张。 */
            s_prefetch_center_valid = true;
            s_prefetch_center_folder = folder;
            s_prefetch_center_index = index;
            s_prefetch_center_generation = s_generation[folder];
            prefetch_plan();
        }
    }
    return true;
}

/** @brief 处理一个槽位的缩略图请求；返回 true 表示本轮做了工作。 */
static bool process_one_thumbnail(void)
{
    if (!s_req_mux) return false;
    for (uint32_t slot = 0; slot < IMAGE_MAX_THUMB_SLOTS; ++slot) {
        thumb_request_t req;
        xSemaphoreTake(s_req_mux, portMAX_DELAY);
        if (!s_thumb_req[slot].pending) {
            xSemaphoreGive(s_req_mux);
            continue;
        }
        req = s_thumb_req[slot];
        s_thumb_req[slot].pending = false;
        uint32_t epoch = s_thumb_clear_epoch;
        xSemaphoreGive(s_req_mux);

        bool ok = false;
        bool from_cache = false;
        bool path_ok = false;
        char path[sizeof(IMAGE_DIR_PICTURES) + IMAGE_NAME_MAX + 4];
        if (ensure_buffers() && req.folder < IMAGE_FOLDER_COUNT &&
            s_ready[req.folder] && req.index < s_count[req.folder] &&
            sd_card_get_generation() == s_generation[req.folder]) {
            path_ok = build_path(req.folder, req.index, path, sizeof(path));
            thumb_mem_t *mem = &s_thumb_mem[slot];
            if (mem->valid && mem->folder == req.folder &&
                mem->index == req.index &&
                mem->generation == s_generation[req.folder]) {
                /* 同一槽位仍是这张图：缓冲未释放，直接重发。 */
                ok = true;
                from_cache = true;
            } else if (path_ok &&
                       thumb_cache_load(THUMB_CACHE_IMG, path, IMAGE_THUMB_W,
                                        IMAGE_THUMB_H, s_thumb_pixels[slot])) {
                /* SD 上的小图命中：只需一次 20KB 读，不再解码原图。 */
                ok = true;
                from_cache = true;
            } else {
                size_t bytes = read_jpeg(req.folder, req.index);
                if (bytes && decode_to_scratch(bytes)) {
                    downscale_460(s_scratch, s_thumb_pixels[slot],
                                  IMAGE_THUMB_W, IMAGE_THUMB_H);
                    ok = true;
                }
            }
        }

        ++s_thumb_total;
        if (from_cache) ++s_thumb_hits;

        /* 同一个槽位可能已经被 UI 换成新图片；只发布最新 token。 */
        xSemaphoreTake(s_req_mux, portMAX_DELAY);
        bool valid = epoch == s_thumb_clear_epoch &&
                     s_thumb_req[slot].token == req.token;
        xSemaphoreGive(s_req_mux);
        /* 无论发布是否已被新请求取代，都要按本次实际写入的内容更新身份，
         * 否则旧身份会让下一次同图请求误用别的像素。 */
        s_thumb_mem[slot] = (thumb_mem_t){
            .valid = ok,
            .folder = req.folder,
            .index = req.index,
            .generation = ok ? s_generation[req.folder] : 0,
        };
        if (valid) publish_thumbnail(slot, req.token, ok);
        /* 只有真正重新解码的结果才写回缓存；命中的小图不必重复写。 */
        if (ok && !from_cache && path_ok)
            thumb_cache_store(THUMB_CACHE_IMG, path, IMAGE_THUMB_W,
                              IMAGE_THUMB_H, s_thumb_pixels[slot]);
        if (s_thumb_total >= IMAGE_MAX_THUMB_SLOTS) {
            ESP_LOGI(TAG, "Thumb cache: %u/%u hits", s_thumb_hits, s_thumb_total);
            s_thumb_hits = 0;
            s_thumb_total = 0;
        }
        return true;
    }
    return false;
}

static void image_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (atomic_exchange(&s_scan_requested, false)) {
            do_scan();
            continue;
        }
        if (process_full()) continue;
        /* 全屏时常驻预取上一张/下一张；普通浏览时先处理缩略图。 */
        if (atomic_load(&s_full_active)) {
            if (process_prefetch()) continue;
        } else if (s_prefetch_center_valid) {
            prefetch_clear();
        }
        if (process_one_thumbnail()) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

/* ============================ 公共接口 ============================ */

esp_err_t image_viewer_init(void)
{
    if (s_task) return ESP_OK;
    s_req_mux = xSemaphoreCreateMutex();
    if (!s_req_mux) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(image_worker, "img_view", IMAGE_WORKER_STACK,
                                NULL, IMAGE_WORKER_PRIO, &s_task, 0) != pdPASS) {
        vSemaphoreDelete(s_req_mux);
        s_req_mux = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void image_viewer_set_ops(const image_viewer_ops_t *ops, void *ctx)
{
    s_ops = ops ? *ops : (image_viewer_ops_t){0};
    s_ops_ctx = ctx;
}

void image_viewer_request_scan(void)
{
    if (!s_task) return;
    atomic_store(&s_scan_requested, true);
    notify_worker();
}

void image_viewer_request_thumbnail(image_folder_t folder, uint32_t index,
                                    uint32_t slot, uint32_t token)
{
    if (!s_task || !s_req_mux || slot >= IMAGE_MAX_THUMB_SLOTS) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_thumb_req[slot].folder = folder;
    s_thumb_req[slot].index = index;
    s_thumb_req[slot].token = token;
    s_thumb_req[slot].pending = true;
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void image_viewer_request_full(image_folder_t folder, uint32_t index,
                               uint32_t token)
{
    if (!s_task || !s_req_mux) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_full_folder = folder;
    s_full_index = index;
    s_full_token = token;
    s_full_pending = true;
    ++s_full_epoch; /* 使任何在途旧解码失效 */
    xSemaphoreGive(s_req_mux);
    notify_worker();
}

void image_viewer_cancel_full(void)
{
    if (!s_req_mux) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    s_full_pending = false;
    ++s_full_epoch;
    xSemaphoreGive(s_req_mux);
}

void image_viewer_clear_thumbs(void)
{
    if (!s_req_mux) return;
    xSemaphoreTake(s_req_mux, portMAX_DELAY);
    for (unsigned i = 0; i < IMAGE_MAX_THUMB_SLOTS; ++i)
        s_thumb_req[i].pending = false;
    ++s_thumb_clear_epoch;
    xSemaphoreGive(s_req_mux);
}

void image_viewer_set_full_active(bool active)
{
    atomic_store(&s_full_active, active);
    notify_worker();
}
