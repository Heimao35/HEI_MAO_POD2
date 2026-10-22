/* .vix 帧位置索引：头部校验、文件大小一致性、检查点查询和损坏拒绝。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#undef free

void *test_heap_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
void *test_heap_calloc(size_t count, size_t size, unsigned caps)
{ (void)caps; return calloc(count, size); }
void *test_heap_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{ (void)alignment; (void)caps; return malloc(size); }
void test_heap_free(void *ptr) { free(ptr); }
void heap_caps_free(void *ptr) { test_heap_free(ptr); }

#include "../../main/video_seek_index.c"

#define TEST_PATH "video-seek-index.vix"

static void put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) fputc((v >> (8 * i)) & 0xFF, f);
}

/* 默认写一个合法索引：1000 帧、25 帧一个检查点、40 项。 */
static void write_index(uint32_t frame_us, uint32_t total, uint32_t interval,
                        uint32_t video_size, uint32_t count,
                        bool corrupt_offsets, bool truncate)
{
    FILE *f = fopen(TEST_PATH, "wb");
    assert(f);
    assert(fwrite("HMVX", 1, 4, f) == 4);
    put16(f, 1);
    put16(f, 0);
    put32(f, frame_us);
    put32(f, total);
    put32(f, interval);
    put32(f, count);
    put32(f, video_size);
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count && !truncate; ++i) {
        uint32_t offset = corrupt_offsets ? previous : i * 49000U;
        put32(f, offset);
        previous = offset;
    }
    assert(fclose(f) == 0);
}

int main(void)
{
    video_seek_index_t *index = NULL;

    /* 合法索引与查询：检查点 k 覆盖 [k*interval, (k+1)*interval)。 */
    write_index(40000, 1000, 25, 2000000, 40, false, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) == ESP_OK);
    assert(index && video_seek_index_frame_us(index) == 40000);
    assert(video_seek_index_total_frames(index) == 1000);
    assert(video_seek_index_interval(index) == 25);
    uint32_t ck = 0;
    uint64_t pos = 0;
    assert(video_seek_index_lookup(index, 0, &ck, &pos) && ck == 0 && pos == 0);
    assert(video_seek_index_lookup(index, 24, &ck, &pos) && ck == 0 && pos == 0);
    assert(video_seek_index_lookup(index, 25, &ck, &pos) && ck == 25 && pos == 49000);
    assert(video_seek_index_lookup(index, 999, &ck, &pos) && ck == 975);
    assert(pos == 39ULL * 49000U);
    assert(video_seek_index_lookup(index, 5000, &ck, &pos) && ck == 975); /* 越界裁剪 */
    video_seek_index_close(index);
    index = NULL;
    puts("PASS VIX header/load and checkpoint lookup");

    /* 文件大小与索引头不一致（视频被替换/重新导出）必须拒绝。 */
    assert(video_seek_index_open(&index, TEST_PATH, 1999999) != ESP_OK && !index);
    /* 头部字段非法：帧周期为 0、总数 0、间隔 0、版本不符。 */
    write_index(0, 1000, 25, 2000000, 40, false, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    write_index(40000, 0, 25, 2000000, 0, false, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    write_index(40000, 1000, 0, 2000000, 40, false, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    /* count 与 total/interval 不匹配。 */
    write_index(40000, 1000, 25, 2000000, 39, false, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    puts("PASS VIX rejects size/version/zero-field/count mismatches");

    /* 偏移必须严格递增且小于视频大小。 */
    write_index(40000, 1000, 25, 2000000, 40, true, false);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    /* 位置表被截断。 */
    write_index(40000, 1000, 25, 2000000, 40, false, true);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) != ESP_OK);
    /* 索引不存在时返回 NOT_FOUND。 */
    remove(TEST_PATH);
    assert(video_seek_index_open(&index, TEST_PATH, 2000000) == ESP_ERR_NOT_FOUND);
    puts("PASS VIX rejects duplicate/out-of-range offsets and truncated tables");

    return 0;
}
