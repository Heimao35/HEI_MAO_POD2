/**
 * @file video_library.c
 * @brief 视频目录扫描、磁盘索引和 4 条一页的读取接口。
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

esp_err_t video_library_make_path_name(const char *name, char *out, size_t size)
{
    if (!name || !name[0] || !out || !size) return ESP_ERR_INVALID_ARG;
    if (strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".."))
        return ESP_ERR_INVALID_ARG;
    int n = snprintf(out, size, "%s/%s", VIDEO_DIR, name);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/*
 * 只把扩展名匹配的普通文件写入索引。
 *
 * FATFS 的 readdir 通常给出 DT_REG/DT_DIR，只有 DT_UNKNOWN 时才再 stat。
 * 扫描阶段不再为每个文件 stat 取大小：视频数量多时，逐个 FAT 目录项
 * stat 会长时间持有 SD 锁；缩略图/播放只在选中的文件上按需 stat。
 */
static bool regular_entry(const char *name, const struct dirent *entry)
{
    if (!video_ext(name) || !strchr(name, '.')) return false;
    if (entry->d_type == DT_REG) return true;
    if (entry->d_type != DT_UNKNOWN) return false;
    char path[VIDEO_NAME_MAX + sizeof(VIDEO_DIR) + 2];
    int n = snprintf(path, sizeof(path), "%s/%s", VIDEO_DIR, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    struct stat st;
    /* 32 位 off_t 让 2 GiB 以上的文件 st_size 变成负值，不能因此跳过；
     * 0 字节文件仍视为无效。 */
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size != 0;
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

        if (!regular_entry(e->d_name, e)) continue;

        video_item_t item;
        memset(&item, 0, sizeof(item));
        if (!copy_name(item.name, e->d_name)) continue;
        /* 大小按需 stat，索引只保存名称；避免扫描时每文件一次 FAT 访问。 */
        item.size = 0;

        if (s_count >= VIDEO_INDEX_MAX_BYTES / sizeof(item)) {
            s_truncated = true;
            break;
        }
        if (fwrite(&item, sizeof(item), 1, index) != 1) {
            ESP_LOGW(TAG, "Video index write failed at %lu: errno=%d",
                     (unsigned long)s_count, errno);
            result = ESP_FAIL;
            break;
        }
        ++s_count;
        if (!(s_count % 128)) ESP_LOGI(TAG, "Indexed %lu videos", (unsigned long)s_count);
    }

    if (cancel && atomic_load(cancel)) result = ESP_ERR_INVALID_STATE;
    if (!sd_card_is_present() && result == ESP_OK) result = ESP_ERR_INVALID_STATE;
    if (fclose(index) != 0) {
        ESP_LOGW(TAG, "Video index close failed: errno=%d", errno);
        if (result == ESP_OK) result = ESP_FAIL;
    }
    closedir(dir);

    if (result != ESP_OK) {
        video_library_reset();
        return result;
    }
    ESP_LOGI(TAG, "Scan: %lu videos, capped=%d", (unsigned long)s_count, s_truncated);
    return ESP_OK;
}

static bool valid_item(const video_item_t *item)
{
    if (!memchr(item->name, 0, sizeof(item->name)) ||
        !item->name[0] || strchr(item->name, '/') || strchr(item->name, '\\') ||
        !strcmp(item->name, "..")) return false;
    /* size 在索引中保留为 0，缩略图/播放阶段按需 stat。 */
    return video_ext(item->name);
}

esp_err_t video_library_get_item(uint32_t index, video_item_t *out)
{
    if (!out || index >= s_count) return ESP_ERR_INVALID_ARG;
    FILE *fp = fopen(INDEX_PATH, "rb");
    if (!fp) return ESP_FAIL;
    bool ok = fseeko(fp, (off_t)(index * sizeof(*out)), SEEK_SET) == 0 &&
              fread(out, sizeof(*out), 1, fp) == 1;
    fclose(fp);
    return ok && valid_item(out) ? ESP_OK : ESP_FAIL;
}

esp_err_t video_library_get_page(uint32_t first, video_page_t *out)
{
    if (!out || first >= s_count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->first = first;

    FILE *fp = fopen(INDEX_PATH, "rb");
    if (!fp) return ESP_FAIL;
    esp_err_t ret = ESP_OK;
    video_item_t item;
    if (fseeko(fp, (off_t)(first * sizeof(item)), SEEK_SET) != 0) {
        ret = ESP_FAIL;
    }
    for (uint32_t i = first; ret == ESP_OK && i < s_count &&
                         out->count < VIDEO_PAGE_SIZE; ++i) {
        if (fread(&item, sizeof(item), 1, fp) != 1 || !valid_item(&item)) {
            ret = ESP_FAIL;
            break;
        }
        memcpy(out->names[out->count], item.name, VIDEO_NAME_MAX);
        out->sizes[out->count] = item.size;
        ++out->count;
    }
    fclose(fp);
    return ret;
}
