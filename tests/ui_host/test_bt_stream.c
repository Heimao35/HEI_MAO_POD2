/* Production Bluetooth PCM stream: 16/24/32-bit conversion, mono->stereo,
 * 44.1<->48 kHz linear resampling, bounded ring and target-rate flush.
 * Single-threaded mutex double; no SDIO/Bluetooth hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../main/bt_audio_stream.c"

static int lock_depth;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore; (void)ticks;
    assert(lock_depth == 0);
    ++lock_depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    assert(lock_depth == 1);
    --lock_depth;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { (void)semaphore; }

static uint8_t read_buf[24 * 1024] __attribute__((aligned(4)));

static void write_s16_stereo(int16_t l, int16_t r, unsigned frames)
{
    int16_t pcm[2 * 256];
    while (frames) {
        unsigned chunk = frames > 256 ? 256 : frames;
        for (unsigned i = 0; i < chunk; ++i) {
            pcm[i * 2] = l;
            pcm[i * 2 + 1] = r;
        }
        assert(bt_stream_write(pcm, chunk, 16, 2, 48000) == chunk);
        frames -= chunk;
    }
}

int main(void)
{
    assert(bt_stream_init() == ESP_OK);
    bt_stream_set_target_rate(48000);

    /* 48k->48k: the streaming linear interpolator has one input-frame
     * latency, so a 100-frame block yields 99 frames; the final frame is
     * emitted when the next block arrives. Samples must otherwise match. */
    bt_stream_reset();
    int16_t ramp[200];
    for (unsigned i = 0; i < 100; ++i) { ramp[i * 2] = (int16_t)i; ramp[i * 2 + 1] = (int16_t)-i; }
    assert(bt_stream_write(ramp, 100, 16, 2, 48000) == 100);
    size_t got = bt_stream_read(read_buf, sizeof(read_buf));
    assert(got == 99 * 4);
    int16_t *out = (int16_t *)read_buf;
    assert(out[0] == 0 && out[1] == 0);
    assert(out[2 * 98] == 98 && out[2 * 98 + 1] == -98);
    int16_t tail[2] = {100, -100};
    assert(bt_stream_write(tail, 1, 16, 2, 48000) == 1);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    assert(got == 4);
    out = (int16_t *)read_buf;
    assert(out[0] == 99 && out[1] == -99);
    puts("PASS 16-bit stereo 48k passthrough preserves samples with one-frame latency");

    /* packed 24-bit little-endian -> top 16 bits, sign extended. */
    bt_stream_reset();
    uint8_t pcm24[6] = {0x56, 0x34, 0x12, 0xBA, 0xDC, 0xFE};
    assert(bt_stream_write(pcm24, 1, 24, 2, 48000) == 1);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    assert(got == 4);
    out = (int16_t *)read_buf;
    assert(out[0] == 0x1234 && out[1] == (int16_t)0xFEDC);
    puts("PASS packed 24-bit stereo converts to signed 16-bit slots");

    /* mono 16-bit is duplicated to both channels. */
    bt_stream_reset();
    int16_t mono = 0x1234;
    assert(bt_stream_write(&mono, 1, 16, 1, 48000) == 1);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    assert(got == 4);
    out = (int16_t *)read_buf;
    assert(out[0] == 0x1234 && out[1] == 0x1234);
    puts("PASS mono 16-bit is duplicated to stereo");

    /* 44.1k -> 48k upsample: output count follows 48000/44100 and stays
     * monotonic for a ramp. */
    bt_stream_reset();
    bt_stream_set_target_rate(48000);
    int16_t ramp441[441 * 2];
    for (unsigned i = 0; i < 441; ++i) {
        ramp441[i * 2] = (int16_t)i;
        ramp441[i * 2 + 1] = (int16_t)-i;
    }
    assert(bt_stream_write(ramp441, 441, 16, 2, 44100) == 441);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    size_t frames = got / 4;
    assert(frames >= 476 && frames <= 482);
    out = (int16_t *)read_buf;
    assert(out[0] == 0);
    for (size_t i = 1; i < frames; ++i)
        assert(out[i * 2] >= out[(i - 1) * 2]);
    assert(out[(frames - 1) * 2] >= 435);
    puts("PASS 44.1k stereo is linearly resampled to about 48k frames");

    /* 48k -> 44.1k downsample still produces the expected frame count. */
    bt_stream_reset();
    bt_stream_set_target_rate(44100);
    int16_t ramp48[480 * 2];
    for (unsigned i = 0; i < 480; ++i) {
        ramp48[i * 2] = (int16_t)i;
        ramp48[i * 2 + 1] = (int16_t)-i;
    }
    assert(bt_stream_write(ramp48, 480, 16, 2, 48000) == 480);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    frames = got / 4;
    assert(frames >= 438 && frames <= 444);
    puts("PASS 48k stereo is resampled to about 44.1k frames");

    /* Bounded ring: overflow is dropped and counted, writer never blocks. */
    bt_stream_reset();
    bt_stream_set_target_rate(48000);
    static int16_t big[5000 * 2];
    for (unsigned i = 0; i < 5000; ++i) { big[i * 2] = (int16_t)i; big[i * 2 + 1] = 0; }
    assert(bt_stream_write(big, 5000, 16, 2, 48000) == 5000);
    assert(bt_stream_queued() == STREAM_RING_BYTES);
    assert(bt_stream_dropped_bytes() > 0);
    got = bt_stream_read(read_buf, sizeof(read_buf));
    assert(got == STREAM_RING_BYTES);
    bt_stream_reset();
    assert(bt_stream_queued() == 0 && bt_stream_dropped_bytes() == 0);
    puts("PASS bounded PCM ring drops overflow instead of blocking");

    /* Changing the target rate flushes queued audio from the old rate. */
    write_s16_stereo(1, 1, 100);
    assert(bt_stream_queued() > 0);
    bt_stream_set_target_rate(44100);
    assert(bt_stream_target_rate() == 44100 && bt_stream_queued() == 0);
    puts("PASS target-rate change flushes the PCM ring");
    return 0;
}
