#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* 实际播放任务唯一拥有解码器、DAC与I2S。以下控制API只投递有界RAM请求，
 * 可在LVGL锁内调用；无文件/I2C等待。选歌使用曲库索引及SD挂载代次。 */
esp_err_t audio_output_init(void);
void audio_output_select(uint32_t index,uint32_t generation,bool autoplay);
void audio_output_toggle(void);
void audio_output_set_playing(bool playing);
void audio_output_set_volume(uint8_t effective_volume);
uint32_t audio_output_time_ms(void);
uint32_t audio_output_duration_sec(void);
bool audio_output_is_playing(void);
/* 已接收的播放意图，包含准备阶段；用于按钮显示/取消，进度仍只读DMA。 */
bool audio_output_is_play_requested(void);
/* Includes pending natural EOS until playlist continuation consumes it. */
bool audio_output_is_active(void);
bool audio_output_take_finished(void);
/* 外部关机/Light-sleep屏障，不改变用户暂停偏好。true表示已停DMA、关闭
 * 文件、停止后续I2C/解码。调用者必须与false配对；不在LVGL锁内等待。 */
bool audio_output_quiesce(bool pause);

/* Light-sleep独立持有者；与关机屏障互不撤销。 */
bool audio_output_sleep(bool pause);

/* ------------------------------------------------------------------ */
/* 视频 PCM 模式：与音乐共用I2S/DAC/ring，不新建通道、不做解码。
 * 进入前调用方必须已暂停音乐（video_player 已执行）。open 同步等待音频
 * worker 配置I2S，失败时视频可静音继续；write 是单生产者接口，视频
 * worker 独占调用；flush 丢弃ring并把音频播放时间归零（seek用）。
 * open/write/flush/close 均不得在LVGL锁内调用。 */
typedef struct {
    uint32_t sample_rate; /* 与音乐相同的支持列表 */
    uint8_t channels;     /* 1/2 */
    uint8_t bits;         /* 16/24/32 */
} audio_output_pcm_cfg_t;

esp_err_t audio_output_video_open(const audio_output_pcm_cfg_t *cfg);
void audio_output_video_close(void);
void audio_output_video_play(bool play);
void audio_output_video_flush(void);
void audio_output_video_eof(void);
size_t audio_output_video_write(const void *pcm,size_t frames,uint32_t timeout_ms);
/* DMA已消费的真实音频帧对应时间；flush后从0开始。 */
uint64_t audio_output_video_time_us(void);
bool audio_output_video_started(void);
bool audio_output_video_play_requested(void);
