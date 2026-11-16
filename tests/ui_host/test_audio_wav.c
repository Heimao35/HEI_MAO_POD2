/* 音乐库 WAV 探测：真实 RIFF/WAVE PCM 16/24/32bit、单双声道、奇数
 * JUNK chunk、data 长度 0/超长裁剪，以及浮动/8bit/错误 block_align 拒绝。
 * 只编译 audio_player.c，不接触 SD/解码器/DAC。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../main/audio_player.c"

static const char *PATH = "audio-wav-probe.wav";

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    assert(fwrite(b, 1, 2, f) == 2);
}
static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8),
                    (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    assert(fwrite(b, 1, 4, f) == 4);
}
static void put_id(FILE *f, const char *id) { assert(fwrite(id, 1, 4, f) == 4); }

/* extra_chunk: 前面插入 3 字节 LIST + 1 字节 padding。
 * data_size_override 用来模拟 0/0xFFFFFFFF/超过文件尾。 */
static void write_wav(uint16_t tag, uint16_t channels, uint32_t rate,
                      uint16_t bits, uint16_t align, uint32_t frames,
                      bool extra_chunk, uint32_t data_size_override)
{
    FILE *f = fopen(PATH, "wb");
    assert(f);
    uint32_t block = align ? align : (uint32_t)channels * (bits / 8U);
    uint32_t data_bytes = frames * block;
    uint32_t extra = extra_chunk ? 12U : 0U;
    put_id(f, "RIFF");
    put32(f, 4U + 8U + 16U + extra + 8U + data_bytes);
    put_id(f, "WAVE");
    if (extra_chunk) {
        put_id(f, "LIST");
        put32(f, 3);
        assert(fwrite("abc", 1, 3, f) == 3);
        assert(fputc(0, f) != EOF);
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
    put32(f, data_size_override ? data_size_override : data_bytes);
    for (uint32_t i = 0; i < data_bytes; ++i) assert(fputc(0x11, f) != EOF);
    fclose(f);
}

/* WAVE_FORMAT_EXTENSIBLE: 40-byte fmt + PCM SubFormat GUID. */
static void write_wav_ext(uint16_t channels, uint32_t rate, uint16_t bits,
                          uint16_t valid_bits, uint16_t align, uint32_t frames,
                          bool pcm_guid)
{
    FILE *f = fopen(PATH, "wb");
    assert(f);
    uint32_t data_bytes = frames * align;
    put_id(f, "RIFF");
    put32(f, 4U + 8U + 40U + 8U + data_bytes);
    put_id(f, "WAVE");
    put_id(f, "fmt ");
    put32(f, 40);
    put16(f, 0xFFFE);
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * align);
    put16(f, align);
    put16(f, bits);
    put16(f, 22);              /* cbSize */
    put16(f, valid_bits);
    put32(f, channels == 2 ? 3U : 4U); /* channel mask */
    uint8_t guid[16] = {0x01,0x00,0x00,0x00,0x00,0x00,0x10,0x00,
                        0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71};
    if (!pcm_guid) guid[0] = 0x03; /* IEEE float subformat */
    assert(fwrite(guid, 1, sizeof(guid), f) == sizeof(guid));
    put_id(f, "data");
    put32(f, data_bytes);
    for (uint32_t i = 0; i < data_bytes; ++i) assert(fputc(0x33, f) != EOF);
    fclose(f);
}

static audio_file_info_t probe(void)
{
    audio_file_info_t info = {0};
    assert(audio_player_probe(PATH, &info) == ESP_OK);
    return info;
}

int main(void)
{
    audio_file_info_t i;

    write_wav(1, 2, 44100, 16, 4, 1000, false, 0);
    i = probe();
    assert(i.is_wav && !i.is_flac && i.metadata_known);
    assert(i.sample_rate == 44100 && i.channels == 2 && i.bits_per_sample == 16);
    assert(i.data_offset == 44 && i.data_bytes == 4000);
    assert(i.total_samples == 1000 && i.duration_ms == 22);
    puts("PASS PCM16 stereo 44100: exact data offset/size/duration");

    write_wav(1, 1, 48000, 24, 3, 640, false, 0);
    i = probe();
    assert(i.sample_rate == 48000 && i.channels == 1 && i.bits_per_sample == 24);
    assert(i.data_bytes == 640 * 3 && i.total_samples == 640);
    puts("PASS PCM24 mono 48000: packed 3-byte frames");

    write_wav(1, 2, 96000, 32, 8, 960, false, 0);
    i = probe();
    assert(i.sample_rate == 96000 && i.channels == 2 && i.bits_per_sample == 32);
    assert(i.data_bytes == 960 * 8 && i.total_samples == 960);
    puts("PASS PCM32 stereo 96000: 8-byte frames");

    /* 24-bit files from DAWs commonly use WAVE_FORMAT_EXTENSIBLE. */
    write_wav_ext(2, 96000, 24, 24, 6, 640, true);
    i = probe();
    assert(i.is_wav && i.sample_rate == 96000 && i.channels == 2 &&
           i.bits_per_sample == 24);
    assert(i.data_offset == 68 && i.data_bytes == 640 * 6);
    puts("PASS WAVE_FORMAT_EXTENSIBLE PCM24 stereo 96000");

    write_wav_ext(2, 96000, 32, 24, 8, 640, true);
    i = probe();
    assert(i.sample_rate == 96000 && i.channels == 2 && i.bits_per_sample == 32);
    assert(i.data_bytes == 640 * 8);
    puts("PASS WAVE_FORMAT_EXTENSIBLE 24-valid-in-32-container");

    write_wav_ext(2, 96000, 24, 24, 6, 640, false);
    {
        audio_file_info_t bad = {0};
        assert(audio_player_probe(PATH, &bad) == ESP_ERR_NOT_SUPPORTED);
    }
    puts("PASS non-PCM extensible subformat rejected");

    write_wav(1, 2, 44100, 16, 4, 512, true, 0);
    i = probe();
    assert(i.data_offset == 56 && i.data_bytes == 512 * 4);
    puts("PASS odd LIST chunk + padding: data chunk located after it");

    write_wav(1, 2, 44100, 16, 4, 100, false, 0);
    i = probe();
    assert(i.data_offset == 44 && i.data_bytes == 400);
    puts("PASS normal data size");

    write_wav(1, 2, 44100, 16, 4, 100, false, 0xFFFFFFFFU);
    i = probe();
    assert(i.data_bytes == 400); /* clamped to file end */
    puts("PASS 0xFFFFFFFF data size clamped to real file end");

    /* A zero size means "until EOF" for streaming writers. */
    {
        FILE *f = fopen(PATH, "wb");
        assert(f);
        put_id(f, "RIFF"); put32(f, 0); put_id(f, "WAVE");
        put_id(f, "fmt "); put32(f, 16); put16(f, 1); put16(f, 2);
        put32(f, 44100); put32(f, 44100 * 4); put16(f, 4); put16(f, 16);
        put_id(f, "data"); put32(f, 0);
        for (unsigned k = 0; k < 200; ++k) assert(fputc(0x22, f) != EOF);
        fclose(f);
    }
    i = probe();
    assert(i.data_bytes == 200 && i.total_samples == 50);
    puts("PASS zero data size falls back to file tail");

    write_wav(3, 2, 44100, 32, 8, 100, false, 0); /* IEEE float */
    {
        audio_file_info_t bad = {0};
        assert(audio_player_probe(PATH, &bad) == ESP_ERR_NOT_SUPPORTED);
    }
    write_wav(1, 2, 44100, 8, 2, 100, false, 0);
    {
        audio_file_info_t bad = {0};
        assert(audio_player_probe(PATH, &bad) == ESP_ERR_NOT_SUPPORTED);
    }
    write_wav(1, 2, 44100, 16, 6, 100, false, 0); /* wrong block_align */
    {
        audio_file_info_t bad = {0};
        assert(audio_player_probe(PATH, &bad) == ESP_ERR_NOT_SUPPORTED);
    }
    write_wav(1, 3, 44100, 16, 6, 100, false, 0); /* 3 channels */
    {
        audio_file_info_t bad = {0};
        assert(audio_player_probe(PATH, &bad) == ESP_ERR_NOT_SUPPORTED);
    }
    puts("PASS float32/8bit/wrong align/3ch rejected");

    remove(PATH);
    puts("audio WAV probe: all checks passed");
    return 0;
}
