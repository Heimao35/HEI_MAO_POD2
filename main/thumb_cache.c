/**
 * @file thumb_cache.c
 * @brief 缩略图磁盘缓存的直接映射槽实现（见 thumb_cache.h）。
 *
 * 每个类型固定槽位数，槽文件固定大小；缓存总量有上限，不需要清理整个
 * 目录，也不会随浏览过的图片数量无限增长。槽内容带源文件大小/修改时间
 * 和像素 CRC，源文件变化或写入中断都会自动退化为未命中。
 */
#include "thumb_cache.h"

#include "sd_card.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "thumb_cache";

/* 测试/工具可以在包含本文件前覆盖根目录；固件默认放在 SD 可丢弃缓存区。 */
#ifndef THUMB_CACHE_ROOT
#define THUMB_CACHE_ROOT SD_SYS_DIR "/.hmcache/thumbs"
#endif

/* 槽数量决定缓存上限：
 *   img  512 x (48 + 100*100*2) = 约 9.8 MiB
 *   home   8 x (48 + 148*148*2) = 约 0.33 MiB
 *   video 128 x (48 + 176*176*2) = 约 7.6 MiB
 * 槽位是直接映射：冲突时后来的覆盖先前的，只会造成一次未命中。 */
#define THUMB_CACHE_IMG_SLOTS   512U
#define THUMB_CACHE_HOME_SLOTS  8U
#define THUMB_CACHE_VIDEO_SLOTS 128U
#define THUMB_CACHE_LOCK_MS     100U
#define THUMB_CACHE_PATH_MAX    128U

/* 缩略图缩放算法、取帧位置或文件头布局变化时递增，旧缓存自动失效。 */
#define THUMB_CACHE_VERSION     1U
#define THUMB_CACHE_FORMAT_RGB565 1U
static const char THUMB_CACHE_MAGIC[4] = {'H', 'M', 'T', '1'};

/* 紧凑的固定头；像素紧跟在头之后。字段按 sizeof 校验，版本变化即可让
 * 旧缓存全部失效（缓存可丢弃，不需要迁移）。 */
typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t kind;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t head_bytes;   /* sizeof(thumb_cache_header_t)，布局变化立即失效 */
    uint32_t data_bytes;
    uint32_t data_crc;
    uint64_t key;
    uint64_t source_size;
    uint32_t source_mtime;
    uint32_t reserved2;
} thumb_cache_header_t;

typedef struct {
    uint16_t slots;
    uint16_t width;
    uint16_t height;
    uint32_t data_bytes;
    const char *dir;
} thumb_kind_info_t;

static const thumb_kind_info_t s_kinds[THUMB_CACHE_KIND_COUNT] = {
    [THUMB_CACHE_IMG] = {
        THUMB_CACHE_IMG_SLOTS, 100, 100, 100U * 100U * 2U,
        THUMB_CACHE_ROOT "/img",
    },
    [THUMB_CACHE_HOME] = {
        THUMB_CACHE_HOME_SLOTS, 148, 148, 148U * 148U * 2U,
        THUMB_CACHE_ROOT "/home",
    },
    [THUMB_CACHE_VIDEO] = {
        THUMB_CACHE_VIDEO_SLOTS, 176, 176, 176U * 176U * 2U,
        THUMB_CACHE_ROOT "/vid",
    },
};

/* 只在 SD 锁内访问；换卡/重新挂载后重新创建目录。 */
static bool s_dir_ready[THUMB_CACHE_KIND_COUNT];
static uint32_t s_dir_generation;

/* ============================ 小工具 ============================ */

static bool make_dir(const char *path)
{
#ifdef _WIN32
    if (mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

static bool ensure_kind_dir(thumb_cache_kind_t kind)
{
    uint32_t generation = sd_card_get_generation();
    if (generation != s_dir_generation) {
        memset(s_dir_ready, 0, sizeof(s_dir_ready));
        s_dir_generation = generation;
    }
    if (s_dir_ready[kind]) return true;
    if (!make_dir(THUMB_CACHE_ROOT) || !make_dir(s_kinds[kind].dir))
        return false;
    s_dir_ready[kind] = true;
    ESP_LOGI(TAG, "Thumbnail cache ready: %s", s_kinds[kind].dir);
    return true;
}

/**
 * @brief 读取源文件的大小/修改时间；大小截断为 32 位。
 *
 * ESP-IDF VFS 的 off_t 是 32 位有符号，2 GiB 以上视频的 st_size 会变成
 * 负值；这里取低 32 位（FAT32 的 fsize 语义），配合修改时间仍能稳定
 * 标识文件。只用于缓存键，不参与任何文件偏移计算。
 */
static bool source_info(const char *path, uint32_t *size, uint32_t *mtime)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size == 0)
        return false;
    *size = (uint32_t)st.st_size;
    *mtime = (uint32_t)st.st_mtime;
    return true;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/** @brief 源路径 + 大小 + 修改时间的 FNV-1a 64 位键。 */
static uint64_t make_key(const char *path, uint32_t size, uint32_t mtime)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = hash_bytes(hash, path, strlen(path));
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(size >> (i * 8));
    hash = hash_bytes(hash, bytes, sizeof(bytes));
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(mtime >> (i * 8));
    hash = hash_bytes(hash, bytes, sizeof(bytes));
    return hash ? hash : 1U;
}

static bool cache_path(thumb_cache_kind_t kind, uint64_t key,
                       char *out, size_t cap)
{
    const thumb_kind_info_t *info = &s_kinds[kind];
    unsigned slot = (unsigned)(key % info->slots);
    int n = snprintf(out, cap, "%s/%04u.bin", info->dir, slot);
    return n > 0 && (size_t)n < cap;
}

/* ============================ 公共接口 ============================ */

bool thumb_cache_load(thumb_cache_kind_t kind, const char *source_path,
                      uint16_t width, uint16_t height, uint8_t *pixels)
{
    if (!pixels || !source_path || kind >= THUMB_CACHE_KIND_COUNT) return false;
    const thumb_kind_info_t *info = &s_kinds[kind];
    if (width != info->width || height != info->height) return false;
    if (!sd_card_is_mounted() || !sd_card_is_present()) return false;
    if (!sd_card_io_lock(pdMS_TO_TICKS(THUMB_CACHE_LOCK_MS))) return false;

    thumb_cache_header_t head;
    memset(&head, 0, sizeof(head));
    uint32_t size = 0, mtime = 0;
    uint64_t key = 0;
    bool read_ok = false;
    if (source_info(source_path, &size, &mtime)) {
        key = make_key(source_path, size, mtime);
        char file[THUMB_CACHE_PATH_MAX];
        if (cache_path(kind, key, file, sizeof(file))) {
            FILE *fp = fopen(file, "rb");
            if (fp) {
                /* 槽文件由本模块独占写入；锁内读到的一定是完整快照。 */
                read_ok = fread(&head, sizeof(head), 1, fp) == 1 &&
                          fread(pixels, 1, info->data_bytes, fp) == info->data_bytes;
                fclose(fp);
            }
        }
    }
    sd_card_io_unlock();
    if (!read_ok) return false;

    /* CRC 放在锁外计算，缩短占用 SD 锁的时间。 */
    return memcmp(head.magic, THUMB_CACHE_MAGIC, sizeof(head.magic)) == 0 &&
           head.version == THUMB_CACHE_VERSION && head.kind == (uint16_t)kind &&
           head.width == width && head.height == height &&
           head.format == THUMB_CACHE_FORMAT_RGB565 &&
           head.head_bytes == sizeof(head) &&
           head.data_bytes == info->data_bytes && head.key == key &&
           head.source_size == (uint64_t)size &&
           head.source_mtime == mtime &&
           esp_crc32_le(0, pixels, info->data_bytes) == head.data_crc;
}

void thumb_cache_store(thumb_cache_kind_t kind, const char *source_path,
                       uint16_t width, uint16_t height, const uint8_t *pixels)
{
    if (!pixels || !source_path || kind >= THUMB_CACHE_KIND_COUNT) return;
    const thumb_kind_info_t *info = &s_kinds[kind];
    if (width != info->width || height != info->height) return;
    if (!sd_card_is_mounted() || !sd_card_is_present()) return;

    /* 像素只读且调用者不再改动；CRC 在拿 SD 锁之前算完。 */
    uint32_t crc = esp_crc32_le(0, pixels, info->data_bytes);
    if (!sd_card_io_lock(pdMS_TO_TICKS(THUMB_CACHE_LOCK_MS))) return;

    uint32_t size = 0, mtime = 0;
    if (source_info(source_path, &size, &mtime) && ensure_kind_dir(kind)) {
        uint64_t key = make_key(source_path, size, mtime);
        char file[THUMB_CACHE_PATH_MAX];
        if (cache_path(kind, key, file, sizeof(file))) {
            FILE *fp = fopen(file, "wb");
            if (fp) {
                thumb_cache_header_t head;
                memset(&head, 0, sizeof(head));
                memcpy(head.magic, THUMB_CACHE_MAGIC, sizeof(head.magic));
                head.version = THUMB_CACHE_VERSION;
                head.kind = (uint16_t)kind;
                head.width = width;
                head.height = height;
                head.format = THUMB_CACHE_FORMAT_RGB565;
                head.head_bytes = sizeof(head);
                head.data_bytes = info->data_bytes;
                head.data_crc = crc;
                head.key = key;
                head.source_size = size;
                head.source_mtime = mtime;
                bool ok = fwrite(&head, sizeof(head), 1, fp) == 1 &&
                          fwrite(pixels, 1, info->data_bytes, fp) == info->data_bytes;
                if (fclose(fp) != 0) ok = false;
                /* 掉电/写满留下不完整文件时删除；下次读取本来也会因 CRC
                 * 或长度失败，删除只是避免继续占用卡空间。 */
                if (!ok) remove(file);
            }
        }
    }
    sd_card_io_unlock();
}
