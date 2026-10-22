/**
 * @file video_library.c
 * @brief 视频目录扫描、磁盘索引和 4 条一页的读取接口。
 *
 * 支持两种布局（UI 显示名不变）：
 * - 文件夹布局：MEDIA/video/<name>/<name>.mjpeg|.mjpg，可选同名 .wav；
 * - 旧式平铺：  MEDIA/video/<name>.mjpeg|.mjpg，无音轨。
 */

#include "video_library.h"
#include "sd_card.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define VIDEO_DIR   SD_SYS_DIR "/MEDIA/video"
#define INDEX_DIR   SD_SYS_DIR "/.hmcache"
#define INDEX_PATH  INDEX_DIR "/video.idx"
/* 文件夹布局最多出现两层各 255 字节的名字，路径缓冲要能容纳两者。 */
#define VIDEO_PATH_MAX (2U * VIDEO_NAME_MAX + sizeof(VIDEO_DIR) + 8U)

/* 索引缓存带有版本头：结构体新增 kind/ext/has_audio 后，旧固件留下的
 * 索引会在首次读取时被判为过期，由播放器触发重新扫描，而不是误读。 */
static const char INDEX_MAGIC[8] = {'H', 'M', 'V', 'I', 'D', '3', 0, 0};
typedef struct {
    char magic[8];
    uint32_t entry_size;
    uint32_t count;
} index_header_t;

static const char *TAG = "video_library";
static uint32_t s_count;
static bool s_truncated;

static bool copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n == 0 || n >= VIDEO_NAME_MAX) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool video_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mjpeg") || !strcasecmp(dot, ".mjpg"));
}

void video_library_reset(void)
{
    s_count = 0;
    s_truncated = false;
}

uint32_t video_library_get_count(void)
{
    return s_count;
}

bool video_library_is_truncated(void)
{
    return s_truncated;
}

esp_err_t video_library_make_video_path(const video_item_t *item,
                                        char *out, size_t size)
{
    if (!item || !out || !size) return ESP_ERR_INVALID_ARG;
    const char *ext = item->ext ? ".mjpg" : ".mjpeg";
    int n;
    if (item->kind == VIDEO_KIND_FOLDER)
        n = snprintf(out, size, "%s/%s/%s%s", VIDEO_DIR, item->name, item->name, ext);
    else
        n = snprintf(out, size, "%s/%s", VIDEO_DIR, item->name);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t video_library_make_audio_path(const video_item_t *item,
                                        char *out, size_t size)
{
    if (!item || !out || !size) return ESP_ERR_INVALID_ARG;
    if (item->kind != VIDEO_KIND_FOLDER || !item->has_audio)
        return ESP_ERR_NOT_FOUND;
    int n = snprintf(out, size, "%s/%s/%s.wav", VIDEO_DIR, item->name, item->name);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t video_library_make_index_path(const video_item_t *item,
                                        char *out, size_t size)
{
    if (!item || !out || !size) return ESP_ERR_INVALID_ARG;
    if (item->kind != VIDEO_KIND_FOLDER) return ESP_ERR_NOT_FOUND;
    int n = snprintf(out, size, "%s/%s/%s.vix", VIDEO_DIR, item->name, item->name);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool regular_nonempty(const char *path)
{
    struct stat st;
    /* 32 位 off_t 让 2 GiB 以上文件的 st_size 变成负值，不能因此跳过；
     * 0 字节文件仍视为无效。 */
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size != 0;
}

/* 文件夹布局：要求 <dir>/<dir>.mjpeg 或 .mjpg 存在，wav 可选。 */
static bool folder_item(const char *name, video_item_t *item)
{
    char path[VIDEO_PATH_MAX];
    for (unsigned ext = 0; ext < 2; ++ext) {
        int n = snprintf(path, sizeof(path), "%s/%s/%s%s", VIDEO_DIR, name, name,
                         ext ? ".mjpg" : ".mjpeg");
        if (n < 0 || (size_t)n >= sizeof(path) || !regular_nonempty(path)) continue;
        memset(item, 0, sizeof(*item));
        if (!copy_name(item->name, name)) return false;
        item->kind = VIDEO_KIND_FOLDER;
        item->ext = (uint8_t)ext;
        n = snprintf(path, sizeof(path), "%s/%s/%s.wav", VIDEO_DIR, name, name);
        item->has_audio = n >= 0 && (size_t)n < sizeof(path) && regular_nonempty(path);
        return true;
    }
    return false;
}

/*
 * 只把扩展名匹配的普通文件写入索引。旧式平铺文件没有音轨。
 *
 * FATFS 的 readdir 通常给出 DT_REG/DT_DIR，只有 DT_UNKNOWN 时才再 stat。
 * 扫描阶段不为每个文件 stat 取大小：缩略图/播放只在选中的文件上按需 stat。
 * 文件夹布局需要一到三次 stat 验证视频/音轨是否存在，数量受目录数限制。
 */
static bool regular_entry(const char *name, const struct dirent *entry)
{
    if (!video_ext(name) || !strchr(name, '.')) return false;
    if (entry->d_type == DT_REG) return true;
    if (entry->d_type != DT_UNKNOWN) return false;
    char path[VIDEO_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", VIDEO_DIR, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    return regular_nonempty(path);
}

static esp_err_t index_open(FILE **out)
{
    *out = NULL;
    FILE *fp = fopen(INDEX_PATH, "rb");
    if (!fp) return ESP_FAIL;
    index_header_t h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || memcmp(h.magic, INDEX_MAGIC, sizeof(h.magic)) ||
        h.entry_size != sizeof(video_item_t) || h.count != s_count) {
        fclose(fp);
        /* 旧版布局/被截断的缓存：让播放器重新扫描，不把它当成 IO 失败。 */
        return ESP_ERR_INVALID_STATE;
    }
    *out = fp;
    return ESP_OK;
}

static bool valid_item(const video_item_t *item)
{
    if (!memchr(item->name, 0, sizeof(item->name)) || !item->name[0] ||
        strchr(item->name, '/') || strchr(item->name, '\\') ||
        !strcmp(item->name, "..") || item->kind > VIDEO_KIND_FOLDER ||
        item->ext > 1) return false;
    return item->kind == VIDEO_KIND_FILE ? video_ext(item->name) : true;
}

static esp_err_t scan_path(FILE *index, const char *name, const video_item_t *item)
{
    if (s_count >= (VIDEO_INDEX_MAX_BYTES - sizeof(index_header_t)) / sizeof(*item)) {
        s_truncated = true;
        return ESP_ERR_INVALID_SIZE;
    }
    if (fwrite(item, sizeof(*item), 1, index) != 1) {
        ESP_LOGW(TAG, "Video index write failed at %lu (%s): errno=%d",
                 (unsigned long)s_count, name, errno);
        return ESP_FAIL;
    }
    ++s_count;
    return ESP_OK;
}

esp_err_t video_library_scan(atomic_bool *cancel)
{
    video_library_reset();
    if (cancel && atomic_load(cancel)) return ESP_ERR_INVALID_STATE;

    DIR *dir = opendir(VIDEO_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s: errno=%d", VIDEO_DIR, errno);
        return ESP_FAIL;
    }

    if (mkdir(INDEX_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Cannot create index directory: errno=%d", errno);
        closedir(dir);
        return ESP_FAIL;
    }
    FILE *index = fopen(INDEX_PATH, "wb");
    if (!index) {
        ESP_LOGE(TAG, "Cannot create video index: errno=%d", errno);
        closedir(dir);
        return ESP_FAIL;
    }
    index_header_t header;
    memcpy(header.magic, INDEX_MAGIC, sizeof(header.magic));
    header.entry_size = sizeof(video_item_t);
    header.count = 0;
    if (fwrite(&header, sizeof(header), 1, index) != 1) {
        fclose(index);
        closedir(dir);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    unsigned visited = 0;
    while (sd_card_is_present()) {
        if (cancel && atomic_load(cancel)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) {
            if (errno) {
                ESP_LOGW(TAG, "Video directory read failed: errno=%d", errno);
                result = ESP_FAIL;
            }
            break;
        }
        /* 每个目录项都可能触发一次 FAT 链读取；定期让出 CPU。 */
        if (!(++visited % 16)) vTaskDelay(1);
        if (e->d_name[0] == '.') continue;

        video_item_t item;
        bool entry_is_dir;
        if (e->d_type == DT_DIR) entry_is_dir = true;
        else if (e->d_type == DT_REG) entry_is_dir = false;
        else if (e->d_type == DT_UNKNOWN) {
            char path[VIDEO_PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s", VIDEO_DIR, e->d_name);
            if (n < 0 || (size_t)n >= sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) entry_is_dir = true;
            else if (S_ISREG(st.st_mode)) entry_is_dir = false;
            else continue;
        } else continue;

        if (entry_is_dir) {
            if (!folder_item(e->d_name, &item)) continue;
        } else {
            if (!regular_entry(e->d_name, e)) continue;
            memset(&item, 0, sizeof(item));
            if (!copy_name(item.name, e->d_name)) continue;
            item.kind = VIDEO_KIND_FILE;
            const char *dot = strrchr(e->d_name, '.');
            item.ext = dot && !strcasecmp(dot, ".mjpg") ? 1 : 0;
            item.has_audio = 0;
        }

        esp_err_t ret = scan_path(index, item.name, &item);
        if (ret == ESP_ERR_INVALID_SIZE) {
            result = ESP_OK; /* 达到索引上限：保留已写入部分并标记截断。 */
            break;
        }
        if (ret != ESP_OK) {
            result = ret;
            break;
        }
        if (!(s_count % 128)) ESP_LOGI(TAG, "Indexed %lu videos", (unsigned long)s_count);
    }

    if (cancel && atomic_load(cancel)) result = ESP_ERR_INVALID_STATE;
    if (!sd_card_is_present() && result == ESP_OK) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {
        header.count = s_count;
        if (fseeko(index, (off_t)offsetof(index_header_t, count), SEEK_SET) != 0 ||
            fwrite(&header.count, sizeof(header.count), 1, index) != 1) {
            ESP_LOGW(TAG, "Video index header update failed: errno=%d", errno);
            result = ESP_FAIL;
        }
    }
    if (fclose(index) != 0) {
        ESP_LOGW(TAG, "Video index close failed: errno=%d", errno);
        if (result == ESP_OK) result = ESP_FAIL;
    }
    closedir(dir);

    if (result != ESP_OK) {
        video_library_reset();
        return result;
    }
    ESP_LOGI(TAG, "Scan: %lu videos, capped=%d",
             (unsigned long)s_count, s_truncated);
    return ESP_OK;
}

esp_err_t video_library_get_item(uint32_t index, video_item_t *out)
{
    if (!out || index >= s_count) return ESP_ERR_INVALID_ARG;
    FILE *fp;
    esp_err_t ret = index_open(&fp);
    if (ret != ESP_OK) return ret;
    off_t offset = (off_t)sizeof(index_header_t) + (off_t)index * (off_t)sizeof(*out);
    bool ok = fseeko(fp, offset, SEEK_SET) == 0 &&
              fread(out, sizeof(*out), 1, fp) == 1;
    fclose(fp);
    return ok && valid_item(out) ? ESP_OK : ESP_FAIL;
}

esp_err_t video_library_get_page(uint32_t first, video_page_t *out)
{
    if (!out || first >= s_count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->first = first;

    FILE *fp;
    esp_err_t ret = index_open(&fp);
    if (ret != ESP_OK) return ret;
    off_t offset = (off_t)sizeof(index_header_t) + (off_t)first * (off_t)sizeof(video_item_t);
    if (fseeko(fp, offset, SEEK_SET) != 0) {
        ret = ESP_FAIL;
    }
    for (uint32_t i = first; ret == ESP_OK && i < s_count &&
                         out->count < VIDEO_PAGE_SIZE; ++i) {
        video_item_t item;
        if (fread(&item, sizeof(item), 1, fp) != 1 || !valid_item(&item)) {
            ret = ESP_FAIL;
            break;
        }
        memcpy(out->names[out->count], item.name, VIDEO_NAME_MAX);
        out->sizes[out->count] = item.size;
        out->items[out->count] = item;
        ++out->count;
    }
    fclose(fp);
    return ret;
}
