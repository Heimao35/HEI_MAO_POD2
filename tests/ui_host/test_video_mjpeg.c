/* Real two-frame 460x460 MJPEG fixture: streaming frame boundaries,
 * seek, and the middle-frame thumbnail extraction path.
 *
 * 生产代码直接调用 FatFs，用 32 位无符号 FSIZE_t 覆盖到 FAT32 上限
 * 4 GiB-1。主机没有 diskio/ff.c，这里用 stdio 实现最小替身：所有路径
 * 都映射到同一个真实 fixture，同时检查 /sdcard 挂载前缀已被去掉。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

/* -------------------------------------------------------------------- */
/* Minimal FatFs test double: stdio-backed, one fixture for every path. */

static const char *s_fixture_path = VIDEO_FIXTURE;
static char s_last_fat_path[VM_PATH_MAX];
static FSIZE_t s_last_seek;
static unsigned s_open_calls, s_stat_calls;

static int test_fseek(FILE *fp, FSIZE_t ofs)
{
#ifdef _WIN32
    return _fseeki64(fp, (long long)ofs, SEEK_SET);
#else
    return fseeko(fp, (off_t)ofs, SEEK_SET);
#endif
}

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
    struct stat st;
    fp->objsize = stat(s_fixture_path, &st) == 0 ? (FSIZE_t)st.st_size : 0;
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
    /* 与 FatFs 只读模式一致：超过文件大小的偏移被裁剪到文件尾。 */
    if (ofs > fp->objsize) ofs = fp->objsize;
    s_last_seek = ofs;
    return test_fseek((FILE *)fp->fp, ofs) == 0 ? FR_OK : 1;
}

FRESULT f_stat(const char *path, FILINFO *fno)
{
    (void)path;
    ++s_stat_calls;
    struct stat st;
    if (stat(s_fixture_path, &st) != 0) return 1;
    fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fno->fsize = (FSIZE_t)st.st_size;
    return FR_OK;
}

int main(void)
{
    assert(video_mjpeg_fps_from_name("clip_24fps.mjpeg") == 24);
    assert(video_mjpeg_fps_from_name("CLIP_30FPS.MJPG") == 30);
    assert(video_mjpeg_fps_from_name("clip_60fps_v2.mjpeg") == 60);
    assert(video_mjpeg_fps_from_name("clip_1080p_24fps.mjpeg") == 24);
    /* 只有分辨率中的 360 不能误判成 60fps。 */
    assert(video_mjpeg_fps_from_name("clip_360p.mjpeg") == 0);
    assert(video_mjpeg_fps_from_name("clip.mjpeg") == 0);

    /* 非挂载点路径必须被拒绝，且不能触碰文件系统。 */
    video_mjpeg_t *bad = NULL;
    assert(video_mjpeg_open(&bad, VIDEO_FIXTURE) == ESP_ERR_NOT_FOUND);
    assert(video_mjpeg_open(&bad, "baseline.mjpeg") == ESP_ERR_NOT_FOUND);
    assert(video_mjpeg_open(&bad, SD_MOUNT_POINT) == ESP_ERR_NOT_FOUND);
    assert(!bad && !s_open_calls && !s_stat_calls);

    /* 播放器/缩略图始终使用 VFS 路径；模块内部必须去掉挂载前缀。 */
    const char *path = SD_MOUNT_POINT "/baseline.mjpeg";
    video_mjpeg_t *reader = NULL;
    assert(video_mjpeg_open(&reader, path) == ESP_OK);
    assert(!strcmp(s_last_fat_path, "/baseline.mjpeg"));
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
    assert(!strcmp(s_last_fat_path, "/baseline.mjpeg"));
    assert(near_bytes > 4 && near_offset < size);
    assert(near[0] == 0xFF && near[1] == 0xD8);
    assert(near[near_bytes - 2] == 0xFF && near[near_bytes - 1] == 0xD9);
    free(near);

    /* FatFs 的 FSIZE_t 是 32 位无符号：2 GiB 之后仍可定位到 4 GiB-1，
     * 这是进度条能跳到超长视频末尾的前提。 */
    video_mjpeg_t *probe = NULL;
    assert(video_mjpeg_open(&probe, path) == ESP_OK);
    probe->size = UINT64_C(0xFFFFFFFF);
    probe->fil.objsize = UINT32_MAX; /* 模拟 4 GiB-1 的文件 */
    assert(video_mjpeg_seek(probe, UINT64_C(0x80000000)));
    assert(s_last_seek == UINT64_C(0x80000000));
    assert(video_mjpeg_seek(probe, UINT64_C(0xFFFFFFFE)));
    assert(s_last_seek == UINT64_C(0xFFFFFE00)); /* 512 字节对齐 */
    video_mjpeg_close(probe);

    puts("PASS video MJPEG parsing/seek/thumbnail with FatFs path and >2GiB seeks");
    return 0;
}
