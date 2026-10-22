/**
 * @file video_wav.c
 * @brief 视频同名 WAV 音轨解析与流式读取（只支持 PCM）。
 */

#include "video_wav.h"
#include "sd_path.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include <string.h>

static const char *TAG = "video_wav";

/* 与视频/调用方一致的路径上限（VIDEO_NAME_MAX+80）。 */
#define WAV_PATH_MAX 336U

struct video_wav {
    FIL fil;
    bool opened;
    uint16_t format;   /* WAVE format tag; only 1 (PCM) is accepted */
    uint32_t rate;
    uint8_t channels;
    uint8_t bits;
    uint16_t block_align;
    uint64_t data_offset;
    uint64_t data_bytes;
    uint64_t frames;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool read_exact(FIL *f, void *buf, size_t bytes)
{
    UINT br = 0;
    return f_read(f, buf, (UINT)bytes, &br) == FR_OK && br == bytes;
}

static bool skip_bytes(FIL *f, uint64_t bytes)
{
    FSIZE_t pos = f_tell(f);
    if (bytes > (uint64_t)UINT32_MAX - pos) return false;
    return f_lseek(f, pos + (FSIZE_t)bytes) == FR_OK;
}

static bool parse_header(FIL *f, video_wav_t *wav)
{
    uint8_t hdr[12];
    if (!read_exact(f, hdr, sizeof(hdr)) || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "WAVE", 4)) return false;

    bool have_fmt = false, have_data = false;
    for (unsigned chunk = 0; chunk < 64; ++chunk) {
        uint8_t ch[8];
        if (!read_exact(f, ch, sizeof(ch))) return false;
        uint32_t size = le32(ch + 4);
        uint64_t body = (uint64_t)f_tell(f);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40];
            size_t take = size < sizeof(fmt) ? size : sizeof(fmt);
            if (size < 16 || !read_exact(f, fmt, take)) return false;
            wav->format = le16(fmt);
            wav->channels = (uint8_t)le16(fmt + 2);
            wav->rate = le32(fmt + 4);
            wav->block_align = le16(fmt + 12);
            wav->bits = (uint8_t)le16(fmt + 14);
            have_fmt = true;
            uint64_t skip = (uint64_t)size - take + (size & 1U);
            if (skip && !skip_bytes(f, skip)) return false;
        } else if (!memcmp(ch, "data", 4)) {
            wav->data_offset = body;
            wav->data_bytes = size;
            have_data = true;
            break;
        } else {
            uint64_t skip = (uint64_t)size + (size & 1U);
            if (!skip_bytes(f, skip)) return false;
        }
    }
    return have_fmt && have_data;
}

esp_err_t video_wav_open(video_wav_t **out, const char *path)
{
    if (!out || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    char fat_path[WAV_PATH_MAX];
    if (!sd_path_to_fatfs(path, fat_path, sizeof(fat_path))) return ESP_ERR_NOT_FOUND;

    /* FIL 自带 FF_MAX_SS 扇区缓存，放 PSRAM 堆对象，避免占用调用方栈。 */
    video_wav_t *wav = heap_caps_calloc(1, sizeof(*wav),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return ESP_ERR_NO_MEM;
    if (f_open(&wav->fil, fat_path, FA_READ) != FR_OK) {
        heap_caps_free(wav);
        return ESP_ERR_NOT_FOUND;
    }
    wav->opened = true;

    if (!parse_header(&wav->fil, wav) || wav->rate == 0 ||
        wav->format != 1 /* PCM only; float/ADPCM need decoding */ ||
        (wav->channels != 1 && wav->channels != 2) ||
        (wav->bits != 16 && wav->bits != 24 && wav->bits != 32) ||
        wav->block_align != (uint16_t)(wav->channels * (wav->bits / 8))) {
        ESP_LOGW(TAG, "Unsupported WAV (PCM 16/24/32bit mono/stereo only)");
        video_wav_close(wav);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 数据长度字段可能是 0/0xFFFFFFFF 或超过实际文件；按文件尾裁剪。 */
    uint64_t file_size = (uint64_t)f_size(&wav->fil);
    if (wav->data_offset > file_size) {
        video_wav_close(wav);
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t available = file_size - wav->data_offset;
    if (wav->data_bytes == 0 || wav->data_bytes > available)
        wav->data_bytes = available;
    wav->frames = wav->data_bytes / wav->block_align;
    if (!wav->frames) {
        video_wav_close(wav);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "WAV: %luHz %ubit %uch, %llu frames",
             (unsigned long)wav->rate, wav->bits, wav->channels,
             (unsigned long long)wav->frames);
    *out = wav;
    return ESP_OK;
}

void video_wav_detach(video_wav_t *wav)
{
    if (!wav) return;
    if (wav->opened) {
        f_close(&wav->fil);
        wav->opened = false;
    }
}

void video_wav_close(video_wav_t *wav)
{
    if (!wav) return;
    video_wav_detach(wav);
    heap_caps_free(wav);
}

uint32_t video_wav_rate(const video_wav_t *wav) { return wav ? wav->rate : 0; }
uint8_t video_wav_channels(const video_wav_t *wav) { return wav ? wav->channels : 0; }
uint8_t video_wav_bits(const video_wav_t *wav) { return wav ? wav->bits : 0; }
uint64_t video_wav_frames(const video_wav_t *wav) { return wav ? wav->frames : 0; }

size_t video_wav_read(video_wav_t *wav, void *pcm, size_t frames)
{
    if (!wav || !wav->opened || !pcm || !frames) return 0;
    /* 调用方每次只读一个小块（几 KiB），frames*block_align 不会超过 UINT。 */
    size_t bytes = frames * wav->block_align;
    UINT br = 0;
    if (f_read(&wav->fil, pcm, (UINT)bytes, &br) != FR_OK) return 0;
    return br / wav->block_align;
}

bool video_wav_seek(video_wav_t *wav, uint64_t frame)
{
    if (!wav || !wav->opened || frame > wav->frames) return false;
    uint64_t offset = wav->data_offset + frame * wav->block_align;
    if (offset > UINT32_MAX) return false; /* FAT32/FSIZE_t 上限 */
    return f_lseek(&wav->fil, (FSIZE_t)offset) == FR_OK;
}
