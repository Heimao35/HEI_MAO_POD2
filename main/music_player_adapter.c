#include "music_player_adapter.h"
#include "player_settings.h"
#include "music_library.h"
#include "audio_player.h"
#include "audio_output.h"
#include "cover_loader.h"
#include "music_background.h"
#include "lyric_parser.h"
#include "sd_card.h"
#include "ui/ui_lockscreen.h"
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
static char *s_lyrics_text;
static music_page_t s_page;
/* 见过一次挂载不代表扫描成功：状态、重试和挂载代次必须分别记录。
 * 这些字段只由单个媒体 worker 访问；UI 仅设置下面的原子请求标志。 */
static uint32_t s_seen_generation = UINT32_MAX;
typedef enum {
    MUSIC_SCAN_PENDING,
    MUSIC_SCAN_WAIT_RETRY,
    MUSIC_SCAN_READY,
    MUSIC_SCAN_FAILED,
} music_scan_state_t;
#define MUSIC_SCAN_MAX_ATTEMPTS 3U
#define MUSIC_SCAN_RETRY_BASE_MS 1000U
static music_scan_state_t s_scan_state = MUSIC_SCAN_PENDING;
static unsigned s_scan_attempts;
static TickType_t s_scan_retry_start;
static TickType_t s_scan_retry_delay;
static atomic_bool s_scan_requested;
static bool s_boot_anim_pending;
static atomic_uint_fast32_t s_requested_index;
/* select 和发布都在 LVGL 锁内：序号用于拒绝先前点击的迟到结果。
 * 请求槽只保留最新索引，既有界，也允许同一首反复请求（包括单曲库）。 */
static uint32_t s_request_serial;
static atomic_bool s_lyrics_ready;

static void notify_worker(void)
{
    if (s_worker) xTaskNotifyGive(s_worker);
}

/* UI 可在解锁回调持有 LVGL 锁时调用。这里只投递请求，不等 SD 锁，
 * 不读写文件；重复请求合并成一个标志，由 worker 判断是否需要补扫。 */
void music_player_adapter_request_scan_if_needed(void)
{
    atomic_store(&s_scan_requested, true);
    notify_worker();
}

/* PAUSED/BUSY 放在同一个原子字中：暂停者和 worker 的 CAS 竞争同一入口，
 * 一旦返回空闲，worker 就不能再开始 I/O，也不会因一次空轮询临时变 BUSY。
 * 两个独立 bool 无法保证这一交接，尤其在 P4 双核同时运行时。 */
#define MEDIA_PAUSED (1U << 0)
#define MEDIA_BUSY   (1U << 1)
static atomic_uint s_worker_state;
static bool media_paused(void) { return (atomic_load(&s_worker_state) & MEDIA_PAUSED) != 0; }
static uint32_t s_selected_index = UINT32_MAX; /* LVGL lock */
static bool s_restore_only; /* only boot restore is paused; user selection plays */
bool music_player_adapter_pause(bool pause)
{
    unsigned previous;
    if (pause) previous = atomic_fetch_or(&s_worker_state, MEDIA_PAUSED);
    else {
        previous = atomic_fetch_and(&s_worker_state, (unsigned)~MEDIA_PAUSED);
        notify_worker();
    }
    bool audio_idle=audio_output_quiesce(pause);
    return (previous & MEDIA_BUSY) == 0 && audio_idle;
}

static bool media_available(void);

static void select_song(uint32_t index)
{
    /* UI callbacks post one scalar index; the media task performs all file I/O. */
    if (media_paused()) return;
    s_selected_index = index;
    audio_output_select(index,sd_card_get_generation(),!s_restore_only);
    ++s_request_serial;
    atomic_store(&s_requested_index, index);
    atomic_store(&s_lyrics_ready, false);
    atomic_store(&s_duration_sec, 0);
    /* 调用方持有 LVGL 锁：仅查内存缓存，不等待 SD 或 JPEG。
     * 命中则本帧开始滑入真实封面，未命中则保持 UI 已清空的区域。 */
    const uint8_t *cover = cover_loader_acquire(sd_card_get_generation(), index);
    if (media_available()) ui_music_player_set_cover(index, cover);
    cover_loader_release(cover);
    /* 背景命中也只查内存；NULL 释放上一选择的待显示图，给 worker 留出
     * 可写槽。已显示背景继续保留，新图后台生成后再平滑过渡。 */
    const uint8_t *background = music_background_acquire(sd_card_get_generation(), index);
    if (media_available()) ui_music_player_set_background(index, background);
    music_background_release(background);
    notify_worker();
}

static uint32_t get_time_ms(void) { return audio_output_time_ms(); }
static uint32_t get_duration_sec(void)
{
    uint32_t duration=audio_output_duration_sec();
    return duration?duration:atomic_load(&s_duration_sec);
}
bool music_player_adapter_is_playing(void) { return audio_output_is_active(); }
static bool is_playing(void) { return audio_output_is_playing(); }

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
    if (media_available() && sd_card_get_generation() == generation)
        ui_music_player_set_page(&s_page);
    lvgl_port_unlock();
}

static void publish_all_names(uint32_t count, uint32_t generation)
{
    /* MUSIC_PAGE_SIZE is only the bounded transport batch. Every song already
     * has a normal row in the original scrolling list. */
    for (uint32_t first = 0; first < count; first += MUSIC_PAGE_SIZE) {
        if (media_paused() || !media_available() || sd_card_get_generation() != generation) return;
        publish_page(first, generation);
    }
}

/* 只在 LVGL 锁内检查请求快照，避免快速选歌/拔卡后的迟到结果发布。 */
static bool request_current(uint32_t index, uint32_t generation, uint32_t serial)
{
    return generation == sd_card_get_generation() && media_available() &&
           serial == s_request_serial && ui_music_player_can_show_song(index);
}

static bool load_selected_song(uint32_t index, uint32_t generation, uint32_t serial)
{
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

    /* 封面先独立发布，不等待音频头/LRC；缓存命中时也不会重复解码。
     * 所有 SD I/O 在 worker 完成，发布前先释放 SD 锁，再获取 LVGL 锁。 */
    if (lock_media(generation) == ESP_OK) {
        item_ret = music_library_get_item(index, &item);
        if (item_ret == ESP_OK && item.cover[0] &&
            music_library_make_path(&item, item.cover, path, sizeof(path)) == ESP_OK)
            cover_ret = cover_loader_load(path, generation, index, &cover);
        sd_card_io_unlock();
    }
    lvgl_port_lock(0);
    bool current = request_current(index, generation, serial);
    if (current) ui_music_player_set_cover(index, cover);
    lvgl_port_unlock();
    if (!current) { cover_loader_release(cover); return false; }

    if (item_ret == ESP_OK && lock_media(generation) == ESP_OK) {
        if (music_library_make_path(&item, item.audio, path, sizeof(path)) == ESP_OK)
            audio_ret = audio_player_probe(path, &info);
        if (s_parser_mutex && xSemaphoreTake(s_parser_mutex, portMAX_DELAY) == pdTRUE) {
            lyric_parser_destroy();
            if (item.lyric[0] && music_library_make_path(&item, item.lyric, path, sizeof(path)) == ESP_OK)
                lyric_ret = lyric_parser_load(path);
            xSemaphoreGive(s_parser_mutex);
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
    current = request_current(index, generation, serial);
    if (current) {
        if (item_ret == ESP_OK) player_settings_set_song(&item);
        uint32_t duration = audio_ret == ESP_OK && info.metadata_known ?
                            (uint32_t)(info.duration_ms / 1000) : 0;
        atomic_store(&s_duration_sec, duration);
        ui_music_player_show_song(index, item_ret == ESP_OK ? item.name : "歌曲不可用",
                                  cover, duration);
        ui_music_player_set_lyrics(lyric_ret == ESP_OK && s_lyrics_text ? s_lyrics_text : "",
                                  lyric_ret == ESP_OK ? lyric_count : 0, lyric_truncated);
        atomic_store(&s_lyrics_ready, lyric_ret == ESP_OK);
    }
    lvgl_port_unlock();
    /* 当前封面、音频信息和歌词已经发布，再生成装饰背景，优先保障选歌。
     * 借用 worker 的封面引用，不再解码 JPG；计算期间不持有 SD/LVGL 锁。
     * 生成后再次验证请求，切歌/拔卡期间产生的旧结果不得覆盖新背景。 */
    const uint8_t *background = NULL;
    if (current && cover) {
        esp_err_t ret = music_background_generate(cover, generation, index, &background);
        if (ret != ESP_OK) ESP_LOGW(TAG, "Background: %s", esp_err_to_name(ret));
        lvgl_port_lock(0);
        current = request_current(index, generation, serial);
        if (current && background) ui_music_player_set_background(index, background);
        lvgl_port_unlock();
    }
    music_background_release(background);
    cover_loader_release(cover); /* UI 的显示引用独立于本次加载引用。 */

    ESP_LOGI(TAG, "song=%lu audio=%s cover=%s lrc=%s duration=%lu",
             (unsigned long)index, esp_err_to_name(audio_ret),
             esp_err_to_name(cover_ret), esp_err_to_name(lyric_ret),
             (unsigned long)atomic_load(&s_duration_sec));
    return current;
}

static void prefetch_neighbors(uint32_t index, uint32_t generation, uint32_t serial)
{
    uint32_t count = music_library_get_count();
    if (count < 2) return;
    uint32_t neighbors[] = {(index + 1) % count, (index + count - 1) % count};
    for (unsigned i = 0; i < 2; ++i) {
        if (i && neighbors[i] == neighbors[0]) continue; /* 两首歌仅预读一次。 */
        /* 每张封面前检查选择是否变化；一张已开始的 JPEG 解码正常结束，
         * 不再启动后续预读。通知保留在任务槽中，下一轮立即处理新选歌。 */
        lvgl_port_lock(0);
        bool current = !media_paused() && request_current(index, generation, serial) &&
                       atomic_load(&s_requested_index) == UINT32_MAX;
        lvgl_port_unlock();
        if (!current) return;
        const uint8_t *cover = cover_loader_acquire(generation, neighbors[i]);
        if (cover) { cover_loader_release(cover); continue; }
        if (lock_media(generation) != ESP_OK) return;
        music_item_t item;
        char path[MUSIC_PATH_MAX];
        if (music_library_get_item(neighbors[i], &item) == ESP_OK && item.cover[0] &&
            music_library_make_path(&item, item.cover, path, sizeof(path)) == ESP_OK) {
            cover_loader_load(path, generation, neighbors[i], &cover);
            cover_loader_release(cover); /* 缓存保留像素，预读不长期持有引用。 */
        }
        sd_card_io_unlock();
    }
}

/* 发布前重新检查挂载代次，避免扫描结束到取得 LVGL 锁之间发生拔插，
 * 将旧卡结果或错误覆盖到新卡。调用时不能持有 SD I/O 锁。 */
static void publish_library_status(uint32_t generation, uint32_t count, const char *status)
{
    lvgl_port_lock(0);
    if (media_available() && sd_card_get_generation() == generation)
        ui_music_player_set_library(count, status);
    lvgl_port_unlock();
}

/* 常驻内存只有两个文件名和一条记录；逐条读取磁盘索引，不把曲库复制到
 * RAM。每条释放 SD 锁，拔卡/关机/用户先选歌时停止；匹配同时验证目录和
 * 音频名，避免重名。没有匹配时采用第一首，不恢复进度或自动播放。 */
static uint32_t restored_song_index(uint32_t count, uint32_t generation)
{
    char folder[MUSIC_NAME_MAX], audio[MUSIC_NAME_MAX];
    lvgl_port_lock(0);
    player_settings_get_song(folder,audio);
    lvgl_port_unlock();
    if (!folder[0] || !audio[0]) return 0;
    for (uint32_t i=0; i<count; ++i) {
        if (media_paused() || atomic_load(&s_requested_index)!=UINT32_MAX) return 0;
        music_item_t item;
        if (lock_media(generation)!=ESP_OK) return 0;
        esp_err_t ret=music_library_get_item(i,&item);
        sd_card_io_unlock();
        if (ret!=ESP_OK) return 0;
        if (!strcmp(folder,item.name) && !strcmp(audio,item.audio)) return i;
        if ((i & 31U)==31U) vTaskDelay(pdMS_TO_TICKS(1));
    }
    return 0;
}

/* 电源任务先暂停 worker 并等其空闲，再调用此函数捕获最后一次点击。
 * 即使用户刚切歌、元数据尚未发布，也保存该曲身份。I/O 在 LVGL 锁外。
 * 无卡时保留原身份，不能用空曲库覆盖已经保存的最后歌曲。 */
esp_err_t music_player_adapter_capture_selection(void)
{
    if (atomic_load(&s_worker_state) != MEDIA_PAUSED) return ESP_ERR_INVALID_STATE;
    lvgl_port_lock(0);
    uint32_t index=s_selected_index;
    lvgl_port_unlock();
    if (index==UINT32_MAX || !media_available()) return ESP_OK;
    uint32_t generation=sd_card_get_generation();
    esp_err_t ret=lock_media(generation);
    if (ret!=ESP_OK) return ret;
    music_item_t item;
    ret=music_library_get_item(index,&item);
    sd_card_io_unlock();
    if (ret==ESP_OK) {
        lvgl_port_lock(0);
        if (sd_card_get_generation()==generation) player_settings_set_song(&item);
        lvgl_port_unlock();
    }
    return ret;
}

/* 单次调度拆为独立函数，既保留单 worker 的串行 I/O，又允许主机测试
 * 确定性模拟首次超时、退避、解锁补扫以及扫描过程中换卡。 */
static void media_worker_step(void)
{
    bool requested = atomic_exchange(&s_scan_requested, false);
    bool available = media_available();
    uint32_t generation = sd_card_get_generation();

    if (!available) {
        if (s_seen_generation != UINT32_MAX) {
            s_seen_generation = UINT32_MAX;
            s_scan_state = MUSIC_SCAN_PENDING;
            s_scan_attempts = 0;
            s_boot_anim_pending = false;
            cover_loader_invalidate();
            music_background_invalidate();
            lvgl_port_lock(0);
            ++s_request_serial;
            s_selected_index=UINT32_MAX;
            audio_output_select(UINT32_MAX,generation,false);
            atomic_store(&s_requested_index, UINT32_MAX);
            atomic_store(&s_lyrics_ready, false);
            atomic_store(&s_duration_sec, 0);
            ui_music_player_set_library(0, "请插入 SD 卡");
            lvgl_port_unlock();
        }
        return;
    }

    if (generation != s_seen_generation) {
        s_seen_generation = generation;
        s_scan_state = MUSIC_SCAN_PENDING;
        s_scan_attempts = 0;
        s_boot_anim_pending = false;
        music_library_reset();
        cover_loader_invalidate();
        music_background_invalidate();
        lvgl_port_lock(0);
        ++s_request_serial;
        s_selected_index=UINT32_MAX;
        audio_output_select(UINT32_MAX,generation,false);
        atomic_store(&s_requested_index, UINT32_MAX);
        atomic_store(&s_lyrics_ready, false);
        atomic_store(&s_duration_sec, 0);
        lvgl_port_unlock();
        publish_library_status(generation, 0, "正在扫描 SD 卡...");
    }

    /* 解锁仅用于兜底：成功（含零首歌曲）绝不重扫，不清空已有曲库或
     * 当前歌曲；失败后允许新一轮有限尝试，退避期间的请求不跳过退避。 */
    if (requested && s_scan_state == MUSIC_SCAN_FAILED) {
        s_scan_state = MUSIC_SCAN_PENDING;
        s_scan_attempts = 0;
        publish_library_status(generation, 0, "正在扫描 SD 卡...");
    }

    if (s_scan_state != MUSIC_SCAN_READY) {
        if (s_scan_state == MUSIC_SCAN_FAILED) return;
        if (s_scan_state == MUSIC_SCAN_WAIT_RETRY &&
            (TickType_t)(xTaskGetTickCount() - s_scan_retry_start) < s_scan_retry_delay) return;

        ++s_scan_attempts;
        esp_err_t ret = lock_media(generation);
        if (ret == ESP_OK) {
            ret = music_library_scan();
            sd_card_io_unlock();
        }
        /* 换卡/拔卡交给下一轮挂载状态处理，不发布旧结果。 */
        if (!media_available() || sd_card_get_generation() != generation) return;

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Music scan attempt %u/%u failed: %s", s_scan_attempts,
                     MUSIC_SCAN_MAX_ATTEMPTS, esp_err_to_name(ret));
            /* A first index-write timeout is followed by repeated EIO on the
             * same controller. Request a detect-task-owned remount; it retries
             * at 20 MHz without recursively clearing VFS under this SD lock. */
            if (s_scan_attempts == 1) sd_card_request_safe_remount();
            if (s_scan_attempts < MUSIC_SCAN_MAX_ATTEMPTS) {
                s_scan_state = MUSIC_SCAN_WAIT_RETRY;
                s_scan_retry_start = xTaskGetTickCount();
                s_scan_retry_delay = pdMS_TO_TICKS(MUSIC_SCAN_RETRY_BASE_MS * s_scan_attempts);
                publish_library_status(generation, 0, "正在等待 SD 卡，稍后重试...");
            } else {
                s_scan_state = MUSIC_SCAN_FAILED;
                publish_library_status(generation, 0, "SD 卡扫描失败");
            }
            return;
        }

        /* 只有扫描真正成功才置 READY，零首歌曲也是成功结果，不能循环重扫。 */
        s_scan_state = MUSIC_SCAN_READY;
        uint32_t count = music_library_get_count();
        bool capped = music_library_is_truncated();
        s_boot_anim_pending = true;
        publish_library_status(generation, count, count ?
                               (capped ? "歌曲列表已达上限" : "正在加载歌曲...") : "未找到歌曲");
        if (count) {
            publish_all_names(count, generation);
            uint32_t restored=restored_song_index(count,generation);
            lvgl_port_lock(0);
            if (media_available() && sd_card_get_generation() == generation &&
                atomic_load(&s_requested_index) == UINT32_MAX && !media_paused()) {
                ui_music_player_restore_selection(restored);
                s_restore_only=true;
                select_song(restored);
                s_restore_only=false;
            }
            lvgl_port_unlock();
        }
        else if (s_boot_anim_pending && ui_lockscreen_is_visible()) {
            /* 空曲库也算扫描成功；没有首首歌曲可等待时，立即启动动画。 */
            s_boot_anim_pending = false;
            ui_lockscreen_start_boot_anim();
        }
        return;
    }

    /* 成功曲库照常处理选歌；与 UI 使用相同的 LVGL 锁取得一致的请求快照。 */
    lvgl_port_lock(0);
    uint32_t index = atomic_exchange(&s_requested_index, UINT32_MAX);
    uint32_t serial = s_request_serial;
    lvgl_port_unlock();
    if (index < music_library_get_count() && load_selected_song(index, generation, serial)) {
        prefetch_neighbors(index, generation, serial);
        /* 首首歌曲的封面、歌词和音频元数据已经读取完成后再启动动画，
         * 避免动画任务与首首歌曲加载再次争用 SD I/O 锁。 */
        if (s_boot_anim_pending && ui_lockscreen_is_visible()) {
            s_boot_anim_pending = false;
            ui_lockscreen_start_boot_anim();
        }
    }
}

static void media_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        unsigned expected = 0;
        if (atomic_compare_exchange_strong(&s_worker_state, &expected, MEDIA_BUSY)) {
            media_worker_step();
            atomic_fetch_and(&s_worker_state, (unsigned)~MEDIA_BUSY);
        }
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
    ret = music_background_init();
    if (ret != ESP_OK) return ret;

    const ui_music_backend_t backend = {
        .select = select_song,
        .page = NULL,
        .rescan = music_player_adapter_request_scan_if_needed,
        .get_time_ms = get_time_ms,
        .get_duration_sec = get_duration_sec,
        .get_lyric_line = get_lyric_line,
        .is_playing = is_playing,
        .set_playing = audio_output_set_playing,
        .is_play_requested = audio_output_is_play_requested,
        .take_finished = audio_output_take_finished,
    };
    atomic_store(&s_requested_index, UINT32_MAX);
    lvgl_port_lock(0);
    ui_music_player_bind(&backend);
    lvgl_port_unlock();
    if (xTaskCreate(media_worker, "sd_music", 12288, NULL, 2, &s_worker) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
