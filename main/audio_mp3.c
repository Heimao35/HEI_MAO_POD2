#include "audio_mp3.h"
#include "esp_mp3_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* 所有接口均由单一 audio_out worker 调用，无 ISR/跨核共享统计。
 * 上板日志已证明 6.5KiB 输入/PCM 暂存没有改善持续吞吐，因此去掉暂存和
 * 包装上下文，完整透传官方 decoder 的 handle、buffer、状态和返回码。
 * 上一轮将运算代码/只读表静态搬入 SRAM，实板发生 I2S DMA 分配不足，
 * 已撤回该实验。这里不改变链接放置，不额外占用静态/动态解码工作区；
 * 必须先确保 SD 和 I2S 必需内存，再评估 MP3 吞吐优化。 */
static audio_mp3_stats_t s_stats;

static esp_audio_err_t timed_decode(void *handle,esp_audio_dec_in_raw_t *raw,
                                    esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info)
{
    TickType_t before=xTaskGetTickCount();
    esp_audio_err_t ret=esp_mp3_dec_decode(handle,raw,out,info);
    TickType_t elapsed=xTaskGetTickCount()-before;
    ++s_stats.calls;s_stats.core_ticks+=elapsed;
    if(elapsed>s_stats.max_core_ticks)s_stats.max_core_ticks=elapsed;
    return ret;
}

esp_audio_err_t audio_mp3_register(void)
{
    /* 注册表只保存指针，ops 必须具有静态生存期。仅 decode 增加计时，
     * 生命周期操作直接使用官方接口，不增加额外分配和输出复制。 */
    static const esp_audio_dec_ops_t ops={
        .open=esp_mp3_dec_open,.decode=timed_decode,
        .reset=esp_mp3_dec_reset,.close=esp_mp3_dec_close};
    return esp_audio_dec_register(ESP_AUDIO_TYPE_MP3,&ops);
}
audio_mp3_stats_t audio_mp3_take_stats(void)
{
    audio_mp3_stats_t result=s_stats;
    memset(&s_stats,0,sizeof(s_stats));
    return result;
}
