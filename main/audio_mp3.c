#include "audio_mp3.h"
#include "esp_audio_dec_reg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <string.h>

/* 固定到 UPSTREAM.txt 中的 commit；只编译 Layer III 标量浮点实现。
 * P4 使用工程的 ilp32f / rv32imafc 工具链，不启用 fast-math，也不强行
 * 把函数/常量放进内部 SRAM。FLAC 和官方 simple parser 不参与替换。 */
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"

typedef struct {
    mp3dec_t decoder;
    /* 使用 PSRAM 中固定的单帧输出，既保证 int16 对齐，也支持公共 API 的
     * 任意字节对齐 buffer。每帧复制最多 4608B，不随歌曲长度增长。
     * 输出容量不足时先返回 needed_size，不能先解码破坏 bit reservoir。 */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} mp3_context_t;

/* 单一 audio_out worker 持有 handle/统计。暂停不 reset，续播保留 reservoir、
 * overlap 和合成滤波状态；切歌/close 释放整块 PSRAM，不另起解码任务。 */
static audio_mp3_stats_t s_stats;

static esp_audio_err_t mp3_open(void *cfg,uint32_t cfg_sz,void **handle)
{
    (void)cfg;(void)cfg_sz;
    if(!handle)return ESP_AUDIO_ERR_INVALID_PARAMETER;
    *handle=NULL;
    mp3_context_t *ctx=heap_caps_malloc(sizeof(*ctx),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!ctx)return ESP_AUDIO_ERR_MEM_LACK;
    memset(ctx,0,sizeof(*ctx));
    mp3dec_init(&ctx->decoder);
    *handle=ctx;
    ESP_LOGI("audio_mp3","MP3 backend: minimp3, PSRAM state=%uB",(unsigned)sizeof(*ctx));
    return ESP_AUDIO_ERR_OK;
}
static esp_audio_err_t mp3_reset(void *handle)
{
    if(!handle)return ESP_AUDIO_ERR_INVALID_PARAMETER;
    mp3_context_t *ctx=handle;
    memset(&ctx->decoder,0,sizeof(ctx->decoder));
    mp3dec_init(&ctx->decoder);
    return ESP_AUDIO_ERR_OK;
}
static esp_audio_err_t mp3_close(void *handle)
{
    if(!handle)return ESP_AUDIO_ERR_INVALID_PARAMETER;
    heap_caps_free(handle);
    return ESP_AUDIO_ERR_OK;
}
static esp_audio_err_t mp3_decode(void *handle,esp_audio_dec_in_raw_t *raw,
                                  esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info)
{
    if(!handle || !raw || !out || !info)return ESP_AUDIO_ERR_INVALID_PARAMETER;
    raw->consumed=0;out->decoded_size=0;out->needed_size=0;
    if((!raw->buffer && raw->len) || raw->len>INT_MAX || !out->buffer)
        return ESP_AUDIO_ERR_INVALID_PARAMETER;
    if(raw->frame_recover!=ESP_AUDIO_DEC_RECOVERY_NONE)return ESP_AUDIO_ERR_NOT_SUPPORT;
    if(!raw->len)return ESP_AUDIO_ERR_OK;
    /* 官方 simple parser 提供完整压缩帧。先协商最大 PCM 帧容量，重试时
     * decoder 状态和输入 consumed 均不变；MPEG1/2/2.5 的实际大小随后发布。 */
    mp3_context_t *ctx=handle;
    if(out->len<sizeof(ctx->pcm)) {
        out->needed_size=sizeof(ctx->pcm);
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    mp3dec_frame_info_t frame={0};
    TickType_t before=xTaskGetTickCount();
    int samples=mp3dec_decode_frame(&ctx->decoder,raw->buffer,(int)raw->len,ctx->pcm,&frame);
    TickType_t elapsed=xTaskGetTickCount()-before;
    ++s_stats.calls;s_stats.core_ticks+=elapsed;
    if(elapsed>s_stats.max_core_ticks)s_stats.max_core_ticks=elapsed;
    if(frame.frame_bytes<0 || (uint32_t)frame.frame_bytes>raw->len)
        return ESP_AUDIO_ERR_FAIL;
    raw->consumed=(uint32_t)frame.frame_bytes;
    if(!frame.frame_bytes)return ESP_AUDIO_ERR_OK;
    if(frame.layer!=3)return ESP_AUDIO_ERR_NOT_SUPPORT;
    /* reservoir 尚未就绪可出现 consumed>0 而 samples=0；继续喂下一帧，
     * 不能 reset、补假进度或把这一帧再次送入 decoder。 */
    if(frame.channels<1 || frame.channels>2 || frame.hz<=0 ||
       samples<0 || samples>MINIMP3_MAX_SAMPLES_PER_FRAME/frame.channels)
        return ESP_AUDIO_ERR_FAIL;
    *info=(esp_audio_dec_info_t){.sample_rate=(uint32_t)frame.hz,.bits_per_sample=16,
        .channel=(uint8_t)frame.channels,.bitrate=(uint32_t)frame.bitrate_kbps*1000,
        .frame_size=(uint32_t)samples*frame.channels*sizeof(mp3d_sample_t)};
    out->decoded_size=info->frame_size;
    if(out->decoded_size)memcpy(out->buffer,ctx->pcm,out->decoded_size);
    return ESP_AUDIO_ERR_OK;
}

esp_audio_err_t audio_mp3_register(void)
{
    /* 注册表仅保存指针，ops 必须静态。只替换 MP3 frame decoder；现有
     * simple parser 继续处理帧边界/跨 read 残片，SD/FLAC/I2S/UI 不变。 */
    static const esp_audio_dec_ops_t ops={
        .open=mp3_open,.decode=mp3_decode,.reset=mp3_reset,.close=mp3_close};
    return esp_audio_dec_register(ESP_AUDIO_TYPE_MP3,&ops);
}
audio_mp3_stats_t audio_mp3_take_stats(void)
{
    audio_mp3_stats_t result=s_stats;
    memset(&s_stats,0,sizeof(s_stats));
    return result;
}
