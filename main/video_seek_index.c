/**
 * @file video_seek_index.c
 * @brief 可选 .vix 帧位置索引的加载与查询（格式见头文件）。
 */

#include "video_seek_index.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "video_index";
#define INDEX_VERSION 1U
#define INDEX_HEADER_BYTES 28U
#define INDEX_MAX_ENTRIES (1024U * 1024U) /* 4 MiB 位置表上限，防御损坏文件 */

struct video_seek_index {
    uint32_t frame_us;
    uint32_t total_frames;
    uint32_t interval;
    uint32_t count;
    uint32_t *offsets;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

esp_err_t video_seek_index_open(video_seek_index_t **out, const char *path,
                                uint32_t video_size)
{
    if (!out || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;

    uint8_t h[INDEX_HEADER_BYTES];
    if (fread(h, 1, sizeof(h), fp) != sizeof(h) || memcmp(h, "HMVX", 4)) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t version = le16(h + 4);
    uint32_t frame_us = le32(h + 8);
    uint32_t total = le32(h + 12);
    uint32_t interval = le32(h + 16);
    uint32_t count = le32(h + 20);
    uint32_t declared_size = le32(h + 24);
    if (version != INDEX_VERSION || !frame_us || !total || !interval ||
        !count || count > INDEX_MAX_ENTRIES || declared_size != video_size ||
        count != (total + interval - 1U) / interval) {
        ESP_LOGW(TAG, "Index rejected: v%u frame_us=%lu total=%lu step=%lu count=%lu size=%lu/%lu",
                 (unsigned)version, (unsigned long)frame_us, (unsigned long)total,
                 (unsigned long)interval, (unsigned long)count,
                 (unsigned long)declared_size, (unsigned long)video_size);
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long size = ftell(fp);
    if (size < 0 || (uint64_t)size != (uint64_t)INDEX_HEADER_BYTES + (uint64_t)count * 4U) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, (long)sizeof(h), SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    video_seek_index_t *index = heap_caps_calloc(1, sizeof(*index),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *offsets = heap_caps_malloc((size_t)count * sizeof(*offsets),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!index || !offsets) {
        heap_caps_free(index);
        heap_caps_free(offsets);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    uint8_t raw[4];
    uint32_t previous = 0;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        if (fread(raw, 1, sizeof(raw), fp) != sizeof(raw)) { valid = false; break; }
        uint32_t offset = le32(raw);
        /* 检查点必须严格递增且落在视频范围内。 */
        if (offset >= video_size || (i && offset <= previous)) { valid = false; break; }
        offsets[i] = offset;
        previous = offset;
    }
    fclose(fp);
    if (!valid) {
        heap_caps_free(index);
        heap_caps_free(offsets);
        return ESP_ERR_INVALID_RESPONSE;
    }
    index->frame_us = frame_us;
    index->total_frames = total;
    index->interval = interval;
    index->count = count;
    index->offsets = offsets;
    ESP_LOGI(TAG, "Index: %lu frames, %lu us/frame, %lu checkpoints",
             (unsigned long)total, (unsigned long)frame_us, (unsigned long)count);
    *out = index;
    return ESP_OK;
}

void video_seek_index_close(video_seek_index_t *index)
{
    if (!index) return;
    heap_caps_free(index->offsets);
    heap_caps_free(index);
}

uint32_t video_seek_index_frame_us(const video_seek_index_t *index)
{ return index ? index->frame_us : 0; }
uint32_t video_seek_index_total_frames(const video_seek_index_t *index)
{ return index ? index->total_frames : 0; }
uint32_t video_seek_index_interval(const video_seek_index_t *index)
{ return index ? index->interval : 0; }

bool video_seek_index_lookup(const video_seek_index_t *index, uint32_t frame,
                             uint32_t *checkpoint_frame, uint64_t *byte_offset)
{
    if (!index || !index->count || !index->offsets || !checkpoint_frame || !byte_offset)
        return false;
    if (frame >= index->total_frames) frame = index->total_frames - 1;
    uint32_t slot = frame / index->interval;
    if (slot >= index->count) slot = index->count - 1;
    *checkpoint_frame = slot * index->interval;
    *byte_offset = index->offsets[slot];
    return true;
}
