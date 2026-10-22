/* Real two-frame 460x460 MJPEG fixture: streaming frame boundaries,
 * seek, and the middle-frame thumbnail extraction path. */
#include <assert.h>
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
    return malloc(size); /* Host stub does not emulate DMA alignment. */
}
void test_heap_free(void *ptr) { free(ptr); }
void heap_caps_free(void *ptr) { test_heap_free(ptr); }

#include "../../main/video_mjpeg.c"

int main(void)
{
    assert(video_mjpeg_fps_from_name("clip_24fps.mjpeg") == 24);
    assert(video_mjpeg_fps_from_name("CLIP_30FPS.MJPG") == 30);
    assert(video_mjpeg_fps_from_name("clip_60fps_v2.mjpeg") == 60);
    assert(video_mjpeg_fps_from_name("clip_1080p_24fps.mjpeg") == 24);
    /* 只有分辨率中的 360 不能误判成 60fps。 */
    assert(video_mjpeg_fps_from_name("clip_360p.mjpeg") == 0);
    assert(video_mjpeg_fps_from_name("clip.mjpeg") == 0);

    /* 32 位 off_t 下 >=2 GiB 的文件 stat 会回绕成负值，必须还原为
     * FATFS 的 32 位无符号大小；FAT32 单文件上限 4 GiB-1。 */
    assert(size_from_stat((off_t)0) == 0);
    assert(size_from_stat((off_t)2147483647) == UINT64_C(2147483647));
    assert(size_from_stat((off_t)-2147483648LL) == UINT64_C(2147483648));
    assert(size_from_stat((off_t)-1) == UINT64_C(4294967295));

    const char *path = VIDEO_FIXTURE;
    video_mjpeg_t *reader = NULL;
    assert(video_mjpeg_open(&reader, path) == ESP_OK);
    assert(reader && video_mjpeg_valid(reader));
    assert(video_mjpeg_size(reader) > 0);

    const uint8_t *frame = NULL;
    size_t bytes = 0;
    uint64_t offset = 0;
    assert(video_mjpeg_next(reader, &frame, &bytes, &offset));
    assert(offset == 0 && bytes > 4);
    assert(frame[0] == 0xFF && frame[1] == 0xD8);
    assert(frame[bytes - 2] == 0xFF && frame[bytes - 1] == 0xD9);
    uint64_t after_first = video_mjpeg_tell(reader);
    uint64_t second_offset;

    assert(video_mjpeg_next(reader, &frame, &bytes, &offset));
    second_offset = offset;
    assert(second_offset >= after_first && bytes > 4);
    assert(frame[0] == 0xFF && frame[1] == 0xD8);
    assert(video_mjpeg_next(reader, &frame, &bytes, &offset) == false);
    assert(video_mjpeg_at_end(reader));

    assert(video_mjpeg_seek(reader, second_offset));
    assert(video_mjpeg_next(reader, &frame, &bytes, &offset));
    assert(offset == second_offset);

    uint64_t size = video_mjpeg_size(reader);
    video_mjpeg_close(reader);

    /* Thumbnail path opens the file itself and returns a complete frame
     * from around the middle of the file. */
    uint8_t *near = malloc(VIDEO_MJPEG_FRAME_MAX);
    assert(near);
    size_t near_bytes = 0;
    uint64_t near_offset = 0;
    assert(video_mjpeg_read_frame_near(path, size / 2, near,
                                        VIDEO_MJPEG_FRAME_MAX,
                                        &near_bytes, &near_offset) == ESP_OK);
    assert(near_bytes > 4 && near_offset < size);
    assert(near[0] == 0xFF && near[1] == 0xD8);
    assert(near[near_bytes - 2] == 0xFF && near[near_bytes - 1] == 0xD9);
    free(near);

    /* 2 GiB 边界：VFS lseek 无法定位之后的位置，必须安全失败；2 GiB 内
     * 的跳转保持原行为（真机 FAT32 单文件上限 4 GiB-1）。 */
    video_mjpeg_t *probe = NULL;
    assert(video_mjpeg_open(&probe, path) == ESP_OK);
    probe->size = UINT64_C(0xFFFFFFFF);
    assert(!video_mjpeg_seek(probe, UINT64_C(0x80000000)));
    assert(video_mjpeg_seek(probe, UINT64_C(0x7FFFFC00)));
    video_mjpeg_close(probe);

    puts("PASS video MJPEG parsing/seek/thumbnail and 2 GiB size recovery");
    return 0;
}
