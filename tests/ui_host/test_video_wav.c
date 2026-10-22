/* 真实 RIFF/WAVE 解析：PCM 16/24/32bit、单双声道、奇数块对齐、
 * 非法格式拒绝以及视频目录内同名音轨的 FatFs 路径转换。
 *
 * 生产代码直连 FatFs；主机用 stdio 实现最小替身并把所有路径映射到
 * 当前写出的临时 WAV 文件。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#undef free

void *test_heap_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}
void *test_heap_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}
void *test_heap_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    (void)alignment;
    (void)caps;
    return malloc(size);
}
void test_heap_free(void *ptr) { free(ptr); }
void heap_caps_free(void *ptr) { test_heap_free(ptr); }

#include "../../main/video_wav.c"

/* -------------------------------------------------------------------- */
/* stdio-backed FatFs double. */

static const char *s_fixture_path = "video-wav-fixture.wav";
static char s_last_fat_path[WAV_PATH_MAX];
static unsigned s_open_calls;

FRESULT f_open(FIL *fp, const char *path, BYTE mode)
{
    (void)mode;
    snprintf(s_last_fat_path, sizeof(s_last_fat_path), "%s", path);
    ++s_open_calls;
    fp->fp = fopen(s_fixture_path, "rb");
    if (!fp->fp) {
        fp->objsize = 0;
        return 1;
    }
    fseek((FILE *)fp->fp, 0, SEEK_END);
    fp->objsize = (FSIZE_t)ftell((FILE *)fp->fp);
    fseek((FILE *)fp->fp, 0, SEEK_SET);
    return FR_OK;
}
FRESULT f_close(FIL *fp)
{
    if (fp->fp) fclose((FILE *)fp->fp);
    fp->fp = NULL;
    return FR_OK;
}
FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br)
{
    *br = (UINT)fread(buf, 1, btr, (FILE *)fp->fp);
    return FR_OK;
}
FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    if (ofs > fp->objsize) ofs = fp->objsize; /* real FatFs clips in FA_READ */
    return fseek((FILE *)fp->fp, (long)ofs, SEEK_SET) == 0 ? FR_OK : 1;
}
FSIZE_t f_tell(FIL *fp) { return (FSIZE_t)ftell((FILE *)fp->fp); }

/* -------------------------------------------------------------------- */

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    assert(fwrite(b, 1, 2, f) == 2);
}
static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    assert(fwrite(b, 1, 4, f) == 4);
}
static void put_id(FILE *f, const char *id) { assert(fwrite(id, 1, 4, f) == 4); }

/* 写入一个可定制 fmt 的 WAV；数据按帧序号填充以便校验定位。 */
static void write_wav(uint16_t tag, uint16_t channels, uint32_t rate,
                      uint16_t bits, uint16_t align, uint32_t frames,
                      bool odd_junk)
{
    FILE *f = fopen(s_fixture_path, "wb");
    assert(f);
    uint32_t block = align ? align : (uint32_t)channels * (bits / 8U);
    uint32_t data_bytes = frames * block;
    uint32_t junk_bytes = odd_junk ? 8U + 3U + 1U : 0U;
    put_id(f, "RIFF");
    put32(f, 4U + 8U + 16U + junk_bytes + 8U + data_bytes);
    put_id(f, "WAVE");
    if (odd_junk) {
        put_id(f, "LIST");
        put32(f, 3);
        assert(fwrite("abc", 1, 3, f) == 3);
        assert(fputc(0, f) != EOF); /* RIFF chunk padding */
    }
    put_id(f, "fmt ");
    put32(f, 16);
    put16(f, tag);
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * block);
    put16(f, align);
    put16(f, bits);
    put_id(f, "data");
    put32(f, data_bytes);
    for (uint32_t i = 0; i < data_bytes; ++i)
        assert(fputc((int)(i & 0xFFU), f) != EOF);
    assert(fclose(f) == 0);
}

static void expect_reject(uint16_t tag, uint16_t channels, uint32_t rate,
                          uint16_t bits, uint16_t align)
{
    write_wav(tag, channels, rate, bits, align, 64, false);
    video_wav_t *wav = NULL;
    assert(video_wav_open(&wav, SD_MOUNT_POINT "/bad.wav") != ESP_OK);
    assert(wav == NULL);
}

int main(void)
{
    /* 非挂载点路径必须被拒绝，且不能触碰文件系统。 */
    video_wav_t *wav = NULL;
    assert(video_wav_open(&wav, "bad.wav") == ESP_ERR_NOT_FOUND);
    assert(video_wav_open(&wav, "/tmp/bad.wav") == ESP_ERR_NOT_FOUND);
    assert(!wav && !s_open_calls);

    /* 48kHz/16bit 立体声；前面放一个奇数长度的 LIST 块验证 pad 跳过。 */
    write_wav(1, 2, 48000, 16, 4, 480, true);
    assert(video_wav_open(&wav, SD_MOUNT_POINT "/track.wav") == ESP_OK);
    assert(!strcmp(s_last_fat_path, "/track.wav"));
    assert(video_wav_rate(wav) == 48000);
    assert(video_wav_channels(wav) == 2 && video_wav_bits(wav) == 16);
    assert(video_wav_frames(wav) == 480);

    uint8_t buf[4096];
    assert(video_wav_read(wav, buf, 100) == 100);
    assert(buf[0] == 0 && buf[1] == 1 && buf[2] == 2 && buf[3] == 3);
    assert(video_wav_read(wav, buf, 1000) == 380); /* EOF 短读 */
    assert(video_wav_read(wav, buf, 10) == 0);

    assert(video_wav_seek(wav, 240));
    assert(video_wav_read(wav, buf, 10) == 10);
    assert(buf[0] == (uint8_t)(240 * 4));
    assert(!video_wav_seek(wav, 481));
    video_wav_close(wav);
    puts("PASS WAV PCM parse/read/seek/path conversion with odd chunk padding");

    /* 24bit 单声道与 32bit 立体声都按 block_align 计算帧数。 */
    write_wav(1, 1, 96000, 24, 3, 320, false);
    assert(video_wav_open(&wav, SD_MOUNT_POINT "/mono.wav") == ESP_OK);
    assert(video_wav_frames(wav) == 320 && video_wav_channels(wav) == 1);
    assert(video_wav_read(wav, buf, 1000) == 320);
    video_wav_close(wav);
    write_wav(1, 2, 192000, 32, 8, 200, false);
    assert(video_wav_open(&wav, SD_MOUNT_POINT "/hires.wav") == ESP_OK);
    assert(video_wav_frames(wav) == 200 && video_wav_bits(wav) == 32);
    video_wav_close(wav);
    puts("PASS WAV 24bit mono / 32bit stereo frame accounting");

    /* 只接受 PCM(1)、16/24/32bit、单/双声道且 block_align 必须匹配。 */
    expect_reject(3, 2, 48000, 16, 4);   /* IEEE float */
    expect_reject(1, 2, 48000, 8, 2);    /* 8bit PCM */
    expect_reject(1, 3, 48000, 16, 6);   /* 3 声道 */
    expect_reject(1, 2, 48000, 16, 6);   /* block_align 错误 */
    puts("PASS WAV rejects non-PCM/8bit/3ch/block-align mismatch");

    /* 数据长度字段超过文件尾时按实际可用字节计算帧数。 */
    write_wav(1, 2, 48000, 16, 4, 100, false);
    FILE *f = fopen(s_fixture_path, "r+b");
    assert(f && fseek(f, 40, SEEK_SET) == 0); /* data size 字段偏移 40 */
    put32(f, 0xFFFFFFFFU);
    assert(fclose(f) == 0);
    assert(video_wav_open(&wav, SD_MOUNT_POINT "/trunc.wav") == ESP_OK);
    assert(video_wav_frames(wav) == 100);
    video_wav_close(wav);
    puts("PASS WAV clamps oversized data length to the file end");

    remove(s_fixture_path);
    return 0;
}
