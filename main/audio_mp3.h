#pragma once
#include <stdint.h>
#include "esp_audio_types.h"

/* MP3 frame decoder 的注册与逐帧计时；官方 simple parser 仍负责帧边界。
 * 单一音频 worker 调用，不允许 ISR 使用。工作区固定驻 PSRAM；时间包含
 * 抢占并按系统 tick 量化，不能解释为独占 CPU 使用时间。 */
typedef struct {
    uint64_t core_ticks;
    uint32_t max_core_ticks,calls;
} audio_mp3_stats_t;
esp_audio_err_t audio_mp3_register(void);
audio_mp3_stats_t audio_mp3_take_stats(void);
