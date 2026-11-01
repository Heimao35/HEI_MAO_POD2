#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "music_library.h"
#include "ui/ui_common.h"

/* U4 扫描事件最多携带 32 字节设备名；多留一个 NUL。 */
#define PLAYER_SETTINGS_BT_NAME_MAX 33

/* 阅读进度采用固定 16 条 LRU 表：key 由书名/TXT 大小/修改时间/排版版本
 * 组成，页码只对同一文件同一排版有效。16×20B 仍使整个设置 blob 保持在
 * 1KiB 内；更久以前的书会被最近阅读的书替换。 */
#define PLAYER_SETTINGS_EBOOK_MAX       16U
#define PLAYER_SETTINGS_EBOOK_MAX_PAGES 32768U
typedef struct {
    uint64_t key;    /**< 书名+TXT大小+修改时间+排版版本 的哈希 */
    uint32_t page;   /**< 0 基页码 */
    uint32_t pages;  /**< 保存时的总页数，用于取值范围校验 */
    uint32_t stamp;  /**< RAM 内 LRU 计数 */
} player_settings_ebook_t;

/* 有界 RAM 快照；不得持久化裸 C 结构体（填充、bool 大小、指针不稳定）。
 * 新增设置：在此增加字段，并在 .c 的 TLV 编解码与 UI 快照处成对接入。
 * song_folder/audio 保存 SD 相对身份，不保存易随扫描变化的歌曲序号。 */
typedef struct {
    uint8_t brightness, volume;
    bool muted, auto_brightness;
    /* 姿态感应设置页两个开关的用户意图。只保存“用户是否希望启用”，
     * 不保存传感器在线状态、当天步数或芯片配置；开机后由 UI 恢复开关
     * 视觉并向 BMI270 worker 重新投递请求。 */
    bool motion_steps, motion_wake;
    /* 最近一次成功配对的蓝牙设备。只保存地址和当时已知的名称；
     * 开关状态按需求不持久化，上电仍默认关闭。 */
    bool bt_paired;
    uint8_t bt_paired_bda[6];
    char bt_paired_name[PLAYER_SETTINGS_BT_NAME_MAX];
    char song_folder[MUSIC_NAME_MAX];
    char song_audio[MUSIC_NAME_MAX];
    /* 最近阅读的电子书进度（LRU）。UI 在 LVGL 锁内只更新这份 RAM 表；
     * 真正写 Flash 仍由电源 worker 的 3 秒合并快照完成。 */
    uint8_t ebook_count;
    player_settings_ebook_t ebook[PLAYER_SETTINGS_EBOOK_MAX];
} player_settings_t;

/* 开机 UI 创建前调用，NVS 失败不擦除分区。 */
esp_err_t player_settings_init(void);
/* 下列 RAM 接口须持有 LVGL 锁；绝不进行 Flash/SD I/O。 */
void player_settings_restore_ui(void);
/* 读取 NVS 快照中的姿态开关意图；仅访问 RAM，不做 Flash I/O。
 * 必须在 ui_manager_init() 之后、持有 LVGL 锁时调用，再交给
 * ui_motion_settings_restore() 套用到开关视觉与传感器请求。 */
void player_settings_get_motion(bool *steps, bool *wake);
/* 读取 NVS 快照中的配对设备；仅访问 RAM。main 在 bt_audio_init 后调用，
 * 把结果交给 bt_audio_set_paired，供“打开蓝牙自动重连”使用。 */
void player_settings_get_bluetooth(bool *valid, uint8_t bda[6],
                                   char *name, size_t name_size);
void player_settings_capture(player_settings_t *out);
void player_settings_get_song(char *folder, char *audio);
void player_settings_set_song(const music_item_t *item);

/* 阅读进度：仅读写 RAM 快照；调用方必须持有 LVGL 锁。
 * get: key 命中且页码有效时返回 true；set: 更新或按 LRU 替换一条记录。
 * Flash 写入仍由电源 worker 的统一快照/3 秒合并保存负责。 */
bool player_settings_get_ebook_progress(uint64_t key, uint32_t *page,
                                        uint32_t *pages);
void player_settings_set_ebook_progress(uint64_t key, uint32_t page,
                                        uint32_t pages);
/* 唯一电源 worker 在 LVGL 锁外调用；成功意味着 commit 和读回校验成功。 */
esp_err_t player_settings_save(const player_settings_t *snapshot);

bool player_settings_equal(const player_settings_t *a, const player_settings_t *b);
