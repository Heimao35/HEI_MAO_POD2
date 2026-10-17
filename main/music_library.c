#include "music_library.h"
#include "sd_card.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define MUSIC_DIR SD_SYS_DIR "/MUSIC"
#define INDEX_PATH SD_SYS_DIR "/.hmcache/music.idx"
static const char *TAG = "music_library";
static uint32_t s_count;
static bool s_truncated;

static bool copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= MUSIC_NAME_MAX) return false;
    memcpy(dst, src, n + 1);
    return true;
}

esp_err_t music_library_make_path(const music_item_t *item, const char *file,
                                  char *out, size_t size)
{
    if (!item || !file || !file[0] || !out) return ESP_ERR_INVALID_ARG;
    int n = snprintf(out, size, "%s/%s/%s", MUSIC_DIR, item->name, file);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool is_type(const char *parent, const struct dirent *entry, bool directory)
{
#ifndef _WIN32
    if (entry->d_type == (directory ? DT_DIR : DT_REG)) return true;
    if (entry->d_type != DT_UNKNOWN) return false;
#endif
    char path[MUSIC_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name);
    struct stat st;
    return n > 0 && n < sizeof(path) && stat(path, &st) == 0 &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

static bool audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".flac") || !strcasecmp(dot, ".mp3"));
}

static bool matching_stem(const char *audio, const char *sidecar)
{
    const char *a = strrchr(audio, '.');
    const char *b = strrchr(sidecar, '.');
    return a && b && (a - audio == b - sidecar) &&
           !strncasecmp(audio, sidecar, a - audio);
}

static bool scan_folder(music_item_t *item)
{
    char folder[MUSIC_PATH_MAX];
    int n = snprintf(folder, sizeof(folder), "%s/%s", MUSIC_DIR, item->name);
    if (n < 0 || n >= sizeof(folder)) return false;
    DIR *dir = opendir(folder);
    if (!dir) return false;
    struct dirent *e;
    unsigned visited = 0;
    /* A folder represents ONE song. Deterministic lexical choice if malformed
     * folders contain multiple audio files; sidecars must match the chosen stem. */
    while (sd_card_is_present() && (e = readdir(dir))) {
        if (!(++visited % 32)) vTaskDelay(1);
        if (!audio_ext(e->d_name) || !is_type(folder, e, false) ||
            strlen(e->d_name) >= MUSIC_NAME_MAX) continue;
        if (!item->audio[0] || strcmp(e->d_name, item->audio) < 0)
            copy_name(item->audio, e->d_name);
    }
    if (!item->audio[0]) { closedir(dir); return false; }
    rewinddir(dir);
    while (sd_card_is_present() && (e = readdir(dir))) {
        if (!(++visited % 32)) vTaskDelay(1);
        if (!matching_stem(item->audio, e->d_name) || !is_type(folder, e, false)) continue;
        const char *ext = strrchr(e->d_name, '.');
        if (!strcasecmp(ext, ".jpg")) copy_name(item->cover, e->d_name);
        if (!strcasecmp(ext, ".lrc")) copy_name(item->lyric, e->d_name);
    }
    closedir(dir);
    return sd_card_is_present();
}

void music_library_reset(void) { s_count = 0; s_truncated = false; }
uint32_t music_library_get_count(void) { return s_count; }
bool music_library_is_truncated(void) { return s_truncated; }

esp_err_t music_library_scan(void)
{
    music_library_reset();
    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) { ESP_LOGW(TAG, "Cannot open %s: errno=%d", MUSIC_DIR, errno); return ESP_FAIL; }
    FILE *index = fopen(INDEX_PATH, "wb");
    if (!index) {
        ESP_LOGE(TAG, "Cannot create bounded disk index (check free space/write protection)");
        closedir(dir); return ESP_FAIL;
    }
    struct dirent *e;
    music_item_t item;
    esp_err_t result = ESP_OK;
    unsigned visited = 0;
    while (sd_card_is_present()) {
        errno = 0;
        e = readdir(dir);
        if (!e) { if (errno) result = ESP_FAIL; break; }
        if (!(++visited % 16)) vTaskDelay(1);
        if (e->d_name[0] == '.' || !is_type(MUSIC_DIR, e, true)) continue;
        memset(&item, 0, sizeof(item));
        if (!copy_name(item.name, e->d_name) || !scan_folder(&item)) continue;
        if (s_count >= MUSIC_INDEX_MAX_BYTES / sizeof(item)) { s_truncated = true; break; }
        if (fwrite(&item, sizeof(item), 1, index) != 1) { result = ESP_FAIL; break; }
        ++s_count;
        if (!(s_count % 128)) ESP_LOGI(TAG, "Indexed %lu songs", (unsigned long)s_count);
    }
    if (!sd_card_is_present()) result = ESP_ERR_INVALID_STATE;
    if (fclose(index) != 0) result = ESP_FAIL;
    closedir(dir);
    if (result != ESP_OK) music_library_reset();
    ESP_LOGI(TAG, "Scan: %lu songs, capped=%d, result=%s", (unsigned long)s_count,
             s_truncated, esp_err_to_name(result));
    return result;
}

static bool valid_item(const music_item_t *item)
{
    /* Cache files are disposable, and must never be trusted as path input. */
    const char *fields[] = {item->name, item->audio, item->cover, item->lyric};
    for (unsigned i = 0; i < 4; ++i) {
        if (!memchr(fields[i], 0, MUSIC_NAME_MAX) || strchr(fields[i], '/') ||
            strchr(fields[i], '\\') || !strcmp(fields[i], "..")) return false;
    }
    return item->name[0] && audio_ext(item->audio);
}

esp_err_t music_library_get_item(uint32_t index, music_item_t *out)
{
    if (!out || index >= s_count) return ESP_ERR_INVALID_ARG;
    FILE *fp = fopen(INDEX_PATH, "rb");
    if (!fp) return ESP_FAIL;
    bool ok = fseek(fp, (long)(index * sizeof(*out)), SEEK_SET) == 0 &&
              fread(out, sizeof(*out), 1, fp) == 1;
    fclose(fp);
    return ok && valid_item(out) ? ESP_OK : ESP_FAIL;
}

esp_err_t music_library_get_page(uint32_t first, music_page_t *out)
{
    if (!out || first >= s_count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->first = first;
    FILE *fp = fopen(INDEX_PATH, "rb");
    if (!fp) return ESP_FAIL;
    esp_err_t ret = ESP_OK;
    music_item_t item;
    if (fseek(fp, (long)(first * sizeof(item)), SEEK_SET) != 0) ret = ESP_FAIL;
    /* One open/seek per page, then sequential reads; no per-row FAT chain walk. */
    for (uint32_t i = first; ret == ESP_OK && i < s_count && out->count < MUSIC_PAGE_SIZE; ++i) {
        if (fread(&item, sizeof(item), 1, fp) != 1 || !valid_item(&item)) { ret = ESP_FAIL; break; }
        memcpy(out->names[out->count++], item.name, MUSIC_NAME_MAX);
    }
    fclose(fp);
    return ret;
}
