#pragma once
#include "esp_err.h"
#include "music_library.h"
#include "ui/ui_common.h"

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
    char song_folder[MUSIC_NAME_MAX];
    char song_audio[MUSIC_NAME_MAX];
} player_settings_t;

/* 开机 UI 创建前调用，NVS 失败不擦除分区。 */
esp_err_t player_settings_init(void);
/* 下列 RAM 接口须持有 LVGL 锁；绝不进行 Flash/SD I/O。 */
void player_settings_restore_ui(void);
/* 读取 NVS 快照中的姿态开关意图；仅访问 RAM，不做 Flash I/O。
 * 必须在 ui_manager_init() 之后、持有 LVGL 锁时调用，再交给
 * ui_motion_settings_restore() 套用到开关视觉与传感器请求。 */
void player_settings_get_motion(bool *steps, bool *wake);
void player_settings_capture(player_settings_t *out);
void player_settings_get_song(char *folder, char *audio);
void player_settings_set_song(const music_item_t *item);
/* 唯一电源 worker 在 LVGL 锁外调用；成功意味着 commit 和读回校验成功。 */
esp_err_t player_settings_save(const player_settings_t *snapshot);

bool player_settings_equal(const player_settings_t *a, const player_settings_t *b);
