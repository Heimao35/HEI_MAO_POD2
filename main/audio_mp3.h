#pragma once
#include <stdint.h>
#include "esp_audio_types.h"

/* 官方 MP3 decoder 的透明计时包装；保留 simple parser 的完整帧边界、
 * bit reservoir 和错误处理。所有接口仅由音频 worker 调用，不允许 ISR 使用。
 * 时间为包含抢占的墙钟 tick，不能解释为 decoder 的独占 CPU 使用时间。 */
typedef struct {
    uint64_t core_ticks;
    uint32_t max_core_ticks,calls;
} audio_mp3_stats_t;
esp_audio_err_t audio_mp3_register(void);
audio_mp3_stats_t audio_mp3_take_stats(void);
