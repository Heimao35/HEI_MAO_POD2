#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sd_card.h"

#define LOCK_ANIMATION_SD_PATH SD_SYS_DIR "/MEDIA/lock_scr/000.mjpeg"
#define LOCK_ANIMATION_MAX_BYTES (4U * 1024U * 1024U)
#define LOCK_ANIMATION_WIDTH 460U
#define LOCK_ANIMATION_HEIGHT 460U
#define LOCK_ANIMATION_FPS 24U

/* 仅在app_main启动音频/电源任务前调用一次，且必须在LVGL锁外。
 * 校验Flash双槽，尝试从已挂载SD导入更换的动画，再发布只读PSRAM缓存。
 * 无卡/源文件无效时沿用有效Flash；运行期间不会重读SD或更新Flash。 */
esp_err_t lock_animation_init(void);
/* 唯一锁屏worker读取；position=0从头重播，返回数据只读且存活至关机。
 * 调用不访问SD/Flash、不分配内存，可安全与启动时的缓存发布并发。 */
bool lock_animation_next_frame(size_t *position, const uint8_t **frame, size_t *bytes);
