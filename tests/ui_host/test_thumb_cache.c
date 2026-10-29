/* Host test for the disposable thumbnail cache: direct-mapped slot files,
 * source size/mtime keys, dimension checks and pixel CRC. Uses the real
 * implementation with a test-local cache root and a RAM SD-lock double. */
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Redirect the production cache root into the CTest working directory. */
#define THUMB_CACHE_ROOT "thumbcache_test"
#include "../../main/thumb_cache.c"

/* ---------------- SD lock/mount doubles ---------------- */
static unsigned lock_calls;
bool sd_card_io_lock(TickType_t timeout)
{
    (void)timeout;
    ++lock_calls;
    return true;
}
void sd_card_io_unlock(void) {}
bool sd_card_is_mounted(void) { return true; }
bool sd_card_is_present(void) { return true; }
uint32_t sd_card_get_generation(void) { return 1; }

/* IDF's esp_crc32_le(0, ...) is the standard reflected CRC-32. */
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, uint32_t bytes)
{
    crc = ~crc;
    while (bytes--) {
        crc ^= *data++;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
    }
    return ~crc;
}

/* ---------------- helpers ---------------- */
static uint8_t img_pixels[100 * 100 * 2];
static uint8_t home_pixels[148 * 148 * 2];
static uint8_t video_pixels[176 * 176 * 2];

static const char *source1 = "thumbcache_test/source1.jpg";
static const char *source2 = "thumbcache_test/source2.jpg";

static unsigned dir_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    unsigned count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] != '.') ++count;
    }
    closedir(d);
    return count;
}

static void clear_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void clear_all(void)
{
    clear_dir("thumbcache_test/img");
    clear_dir("thumbcache_test/home");
    clear_dir("thumbcache_test/vid");
    remove(source1);
    remove(source2);
    rmdir("thumbcache_test");
}

static void write_source(const char *path, unsigned bytes, unsigned seed)
{
    FILE *fp = fopen(path, "wb");
    assert(fp);
    for (unsigned i = 0; i < bytes; ++i) fputc((int)((i * 31U + seed) & 0xFFU), fp);
    assert(fclose(fp) == 0);
}

static bool corrupt_cache_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    bool done = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strstr(e->d_name, ".bin")) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *fp = fopen(path, "r+b");
        assert(fp);
        /* 头部之后任意一个像素字节；CRC 必须因此失配。
         * 源文件变化后可能同时存在新旧两个槽文件，全部破坏才能保证
         * 当前键对应的条目一定被覆盖到。 */
        assert(fseek(fp, (long)sizeof(thumb_cache_header_t) + 17L, SEEK_SET) == 0);
        fputc(0xFF, fp);
        assert(fclose(fp) == 0);
        done = true;
    }
    closedir(d);
    return done;
}

int main(void)
{
    clear_all();
    assert(mkdir("thumbcache_test") == 0); /* MinGW host harness */

    write_source(source1, 4096, 1);
    write_source(source2, 4096, 2);
    for (unsigned i = 0; i < sizeof(img_pixels); ++i) img_pixels[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof(home_pixels); ++i) home_pixels[i] = (uint8_t)(i * 3U);
    for (unsigned i = 0; i < sizeof(video_pixels); ++i) video_pixels[i] = (uint8_t)(i * 7U);

    /* 初始没有缓存；store 之后应能原样读回，并且只占用一个槽文件。 */
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    thumb_cache_store(THUMB_CACHE_IMG, source1, 100, 100, img_pixels);
    assert(lock_calls > 0);
    assert(dir_files("thumbcache_test/img") == 1);
    memset(img_pixels, 0, sizeof(img_pixels));
    assert(thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    assert(img_pixels[0] == 0 && img_pixels[1] == 1 && img_pixels[12345] == (uint8_t)12345);
    puts("PASS thumbnail cache stores/loads RGB565 and uses one slot file");

    /* 尺寸是缓存格式的一部分：请求错误尺寸必须未命中。 */
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source1, 101, 100, img_pixels));
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 99, img_pixels));
    puts("PASS dimension mismatch is a cache miss");

    /* 源文件变化（大小/修改时间）后旧小图不得再被使用。 */
    assert(thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    FILE *fp = fopen(source1, "ab");
    assert(fp);
    fputc('x', fp);
    assert(fclose(fp) == 0);
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    thumb_cache_store(THUMB_CACHE_IMG, source1, 100, 100, img_pixels);
    assert(thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    puts("PASS source size/mtime changes invalidate the entry");

    /* 不同路径不能复用同一个槽（键不同）。 */
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source2, 100, 100, img_pixels));
    puts("PASS different source path is a miss");

    /* 位翻转/截断由 CRC 捕获，且下一次 store 会修复该槽。 */
    assert(corrupt_cache_files("thumbcache_test/img"));
    assert(!thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    thumb_cache_store(THUMB_CACHE_IMG, source1, 100, 100, img_pixels);
    assert(thumb_cache_load(THUMB_CACHE_IMG, source1, 100, 100, img_pixels));
    puts("PASS pixel CRC rejects corrupted cache files");

    /* 三种缩略图使用独立目录/尺寸，互不覆盖。 */
    thumb_cache_store(THUMB_CACHE_HOME, source1, 148, 148, home_pixels);
    thumb_cache_store(THUMB_CACHE_VIDEO, source1, 176, 176, video_pixels);
    memset(home_pixels, 0, sizeof(home_pixels));
    memset(video_pixels, 0, sizeof(video_pixels));
    assert(thumb_cache_load(THUMB_CACHE_HOME, source1, 148, 148, home_pixels));
    assert(thumb_cache_load(THUMB_CACHE_VIDEO, source1, 176, 176, video_pixels));
    assert(home_pixels[17] == (uint8_t)(17 * 3U));
    assert(video_pixels[19] == (uint8_t)(19 * 7U));
    assert(dir_files("thumbcache_test/home") == 1);
    assert(dir_files("thumbcache_test/vid") == 1);
    puts("PASS image/home/video caches are independent");

    clear_all();
    puts("ALL THUMBNAIL CACHE TESTS PASSED");
    return 0;
}
