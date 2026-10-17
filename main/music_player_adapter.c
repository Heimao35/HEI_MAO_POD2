#include "music_player_adapter.h"
#include "music_library.h"
#include "audio_player.h"
#include "cover_loader.h"
#include "lyric_parser.h"
#include "sd_card.h"
#include "ui/ui_music_player.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "music_adapter";
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_parser_mutex;
static atomic_uint_fast32_t s_duration_sec;
static atomic_uint_fast32_t s_time_ms;
static atomic_bool s_playing;
static char *s_lyrics_text;
static music_page_t s_page;
static uint32_t s_seen_generation = UINT32_MAX;
static atomic_uint_fast32_t s_requested_index;
/* select 和发布都在 LVGL 锁内：序号用于拒绝先前点击的迟到结果。
 * 请求槽只保留最新索引，既有界，也允许同一首反复请求（包括单曲库）。 */
static uint32_t s_request_serial;
static atomic_bool s_lyrics_ready;

static void notify_worker(void)
{
    if (s_worker) xTaskNotifyGive(s_worker);
}

static void select_song(uint32_t index)
{
    /* UI callbacks post one scalar index; the media task performs all file I/O. */
    ++s_request_serial;
    atomic_store(&s_requested_index, index);
    notify_worker();
}

static uint32_t get_time_ms(void) { return atomic_load(&s_time_ms); }
static uint32_t get_duration_sec(void) { return atomic_load(&s_duration_sec); }
static bool is_playing(void) { return atomic_load(&s_playing); }

static uint32_t get_lyric_line(uint32_t time_ms)
{
    if (!atomic_load(&s_lyrics_ready) || !s_parser_mutex || xSemaphoreTake(s_parser_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return UINT32_MAX;
    uint32_t line = lyric_parser_get_current_line(time_ms);
    xSemaphoreGive(s_parser_mutex);
    return line;
}

static bool media_available(void)
{
    return sd_card_is_mounted() && sd_card_is_present();
}

static esp_err_t lock_media(uint32_t generation)
{
    if (!sd_card_io_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    if (!media_available() || sd_card_get_generation() != generation) {
        sd_card_io_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void publish_page(uint32_t first, uint32_t generation)
{
    if (lock_media(generation) != ESP_OK) return;
    esp_err_t ret = music_library_get_page(first, &s_page);
    sd_card_io_unlock();
    if (ret != ESP_OK) return;

    lvgl_port_lock(0);
    ui_music_player_set_page(&s_page);
    lvgl_port_unlock();
}

static void publish_all_names(uint32_t count, uint32_t generation)
{
    /* MUSIC_PAGE_SIZE is only the bounded transport batch. Every song already
     * has a normal row in the original scrolling list. */
    for (uint32_t first = 0; first < count; first += MUSIC_PAGE_SIZE) {
        if (!media_available() || sd_card_get_generation() != generation) return;
        publish_page(first, generation);
    }
}

static void load_selected_song(uint32_t index, uint32_t generation, uint32_t serial)
{
    /* 解析器会被本次加载替换；发布前禁止 UI 用新时间索引定位旧歌词。 */
    atomic_store(&s_lyrics_ready, false);
    if (s_lyrics_text) s_lyrics_text[0] = '\0';
    music_item_t item = {0};
    audio_file_info_t info = {0};
    char path[MUSIC_PATH_MAX];
    const uint8_t *cover = NULL;
    esp_err_t item_ret = ESP_FAIL;
    esp_err_t audio_ret = ESP_FAIL;
    esp_err_t cover_ret = ESP_ERR_NOT_FOUND;
    esp_err_t lyric_ret = ESP_ERR_NOT_FOUND;

    if (lock_media(generation) == ESP_OK) {
        item_ret = music_library_get_item(index, &item);
        if (item_ret == ESP_OK) {
            if (music_library_make_path(&item, item.audio, path, sizeof(path)) == ESP_OK)
                audio_ret = audio_player_probe(path, &info);
            if (item.cover[0] && music_library_make_path(&item, item.cover, path, sizeof(path)) == ESP_OK)
                cover_ret = cover_loader_load(path, &cover);

            if (s_parser_mutex && xSemaphoreTake(s_parser_mutex, portMAX_DELAY) == pdTRUE) {
                lyric_parser_destroy();
                if (item.lyric[0] && music_library_make_path(&item, item.lyric, path, sizeof(path)) == ESP_OK)
                    lyric_ret = lyric_parser_load(path);
                xSemaphoreGive(s_parser_mutex);
            }
        }
        sd_card_io_unlock();
    }

    uint32_t lyric_count = 0;
    bool lyric_truncated = false;
    if (s_parser_mutex && xSemaphoreTake(s_parser_mutex, portMAX_DELAY) == pdTRUE) {
        lyric_count = lyric_parser_get_line_count();
        lyric_truncated = lyric_parser_is_truncated();
        if (s_lyrics_text) lyric_parser_copy_all(s_lyrics_text, LYRIC_TEXT_BYTES + 1);
        xSemaphoreGive(s_parser_mutex);
    }

    lvgl_port_lock(0);
    /* 拔卡、重挂载或新选择发生后，丢弃旧结果。动画滑出期间也不能覆盖旧图。
     * 未 commit 的 JPEG 缓冲可被下一请求复用，不会覆盖正在显示的缓冲。 */
    if (generation == sd_card_get_generation() && media_available() &&
        serial == s_request_serial && ui_music_player_can_show_song(index)) {
        uint32_t duration = audio_ret == ESP_OK && info.metadata_known ?
                            (uint32_t)(info.duration_ms / 1000) : 0;
        atomic_store(&s_duration_sec, duration);
        atomic_store(&s_time_ms, 0);
        atomic_store(&s_playing, false);
        ui_music_player_show_song(index, item_ret == ESP_OK ? item.name : "Song unavailable",
                                  cover_ret == ESP_OK ? cover : NULL, duration);
        ui_music_player_set_lyrics(lyric_ret == ESP_OK && s_lyrics_text ? s_lyrics_text : "",
                                  lyric_ret == ESP_OK ? lyric_count : 0, lyric_truncated);
        if (cover_ret == ESP_OK) cover_loader_commit(cover);
        atomic_store(&s_lyrics_ready, lyric_ret == ESP_OK);
        /* 失败时也用占位结束等待，避免封面永远留在屏幕外。 */
        ui_music_player_song_ready(index);
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "song=%lu audio=%s cover=%s lrc=%s duration=%lu",
             (unsigned long)index, esp_err_to_name(audio_ret),
             esp_err_to_name(cover_ret), esp_err_to_name(lyric_ret),
             (unsigned long)atomic_load(&s_duration_sec));
}

static void media_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        bool available = media_available();
        uint32_t generation = sd_card_get_generation();

        if (available && generation != s_seen_generation) {
            s_seen_generation = generation;
            music_library_reset();
            lvgl_port_lock(0);
            ++s_request_serial;
            atomic_store(&s_requested_index, UINT32_MAX);
            atomic_store(&s_lyrics_ready, false);
            atomic_store(&s_duration_sec, 0);
            atomic_store(&s_time_ms, 0);
            atomic_store(&s_playing, false);
            ui_music_player_set_library(0, "Scanning SD card...");
            lvgl_port_unlock();

            if (lock_media(generation) == ESP_OK) {
                esp_err_t ret = music_library_scan();
                uint32_t count = music_library_get_count();
                bool capped = music_library_is_truncated();
                sd_card_io_unlock();
                if (ret == ESP_OK && count) {
                    lvgl_port_lock(0);
                    ui_music_player_set_library(count, capped ? "Song list capped" : "Loading songs...");
                    lvgl_port_unlock();
                    publish_all_names(count, generation);
                    /* Let the normal request path load the first song once;
                     * this avoids decoding the first cover/LRC twice. */
                    lvgl_port_lock(0);
                    /* 如果发布歌名期间用户已经点了歌曲，保留用户选择。 */
                    if (atomic_load(&s_requested_index) == UINT32_MAX) select_song(0);
                    lvgl_port_unlock();
                } else {
                    lvgl_port_lock(0);
                    ui_music_player_set_library(0, ret == ESP_OK ? "No songs found" : "SD scan failed");
                    lvgl_port_unlock();
                }
            }
            continue;
        }

        if (!available) {
            if (s_seen_generation != UINT32_MAX) {
                s_seen_generation = UINT32_MAX;
                lvgl_port_lock(0);
                ++s_request_serial;
                atomic_store(&s_requested_index, UINT32_MAX);
                atomic_store(&s_lyrics_ready, false);
                atomic_store(&s_duration_sec, 0);
                atomic_store(&s_time_ms, 0);
                atomic_store(&s_playing, false);
                ui_music_player_set_library(0, "Insert SD card");
                lvgl_port_unlock();
            }
            continue;
        }

        /* 与 select 共用 LVGL 锁，使索引和请求序号成为一致的快照。
         * 用 exchange 消费请求，不能按歌曲索引去重，否则重复选同一首会卡住转场。 */
        lvgl_port_lock(0);
        uint32_t index = atomic_exchange(&s_requested_index, UINT32_MAX);
        uint32_t serial = s_request_serial;
        lvgl_port_unlock();
        if (index < music_library_get_count()) load_selected_song(index, generation, serial);
    }
}

esp_err_t music_player_adapter_init(void)
{
    if (s_worker) return ESP_ERR_INVALID_STATE;
    s_parser_mutex = xSemaphoreCreateMutex();
    if (!s_parser_mutex) return ESP_ERR_NO_MEM;
    s_lyrics_text = heap_caps_calloc(1, LYRIC_TEXT_BYTES + 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lyrics_text) return ESP_ERR_NO_MEM;
    esp_err_t ret = cover_loader_init();
    if (ret != ESP_OK) return ret;

    const ui_music_backend_t backend = {
        .select = select_song,
        .page = NULL,
        .rescan = NULL,
        .get_time_ms = get_time_ms,
        .get_duration_sec = get_duration_sec,
        .get_lyric_line = get_lyric_line,
        .is_playing = is_playing,
    };
    lvgl_port_lock(0);
    ui_music_player_bind(&backend);
    lvgl_port_unlock();
    atomic_store(&s_requested_index, UINT32_MAX);
    if (xTaskCreate(media_worker, "sd_music", 12288, NULL, 2, &s_worker) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
