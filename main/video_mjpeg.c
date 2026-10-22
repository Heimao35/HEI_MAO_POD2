/**
 * @file video_mjpeg.c
 * @brief 裸 MJPEG 流式帧解析与缩略图取帧。
 *
 * 解析原则与锁屏动画一致：JPEG 段必须按长度跳过，熵编码区只接受 FF00、
 * RST0..7 和 FFD9。这样不会被 APP 段内嵌缩略图的 FFD9 提前截断。
 *
 * 文件访问直接调用 FatFs，而不是 VFS：ESP-IDF 的 off_t 是 32 位有符号，
 * VFS 只能定位到 2 GiB；FatFs 的 FSIZE_t 是 32 位无符号，可覆盖 FAT32
 * 单文件上限 4 GiB-1，进度条因此能跳转到文件末尾附近。
 */

#include "video_mjpeg.h"
#include "sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "video_mjpeg";

/* 与调用方的路径缓冲（VIDEO_NAME_MAX+80）一致；超长路径直接拒绝。 */
#define VM_PATH_MAX 336U

struct video_mjpeg {
    FIL fil;                 /* FatFs 文件对象；opened 为 true 时有效 */
    bool opened;             /* detach/close 后清零，避免再次 f_close */
    uint64_t size;
    uint64_t read_base;      /* read_buf[0] 对应的文件偏移 */
    uint64_t next_read;      /* 下一次 f_read 的文件偏移 */
    size_t read_len;
    size_t read_pos;
    uint8_t *read_buf;
    uint8_t *frame_buf;
    size_t frame_len;
    bool eof;
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static bool contains_ci(const char *text, const char *needle)
{
    if (!text || !needle) return false;
    size_t n = strlen(needle);
    if (!n) return false;
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) ==
               tolower((unsigned char)needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

uint8_t video_mjpeg_fps_from_name(const char *name)
{
    if (!name) return 0;
    /* 只接受带 fps 的显式标记，避免把 360p/1080p 之类的分辨率误判。 */
    if (contains_ci(name, "24fps")) return 24;
    if (contains_ci(name, "30fps")) return 30;
    if (contains_ci(name, "60fps")) return 60;
    return 0;
}

/*
 * 直接调用 FatFs f_read，不要用 stdio fread()。
 *
 * 本项目的音频路径已经验证过：v5.5.4 的 stdio 缓冲/小块路径会显著放大
 * SD 命令开销；f_read 由调用方给出完整块大小，FatFs 才能把 64 KiB 的
 * 读缓冲按扇区直接交给 sdmmc_read_sectors() 做多块 DMA。走 VFS 时底层
 * 也是 f_read，这里少一层路径/描述符转发，读写性能保持不变。
 */
static size_t read_full(FIL *file, void *buf, size_t bytes)
{
    size_t done = 0;
    while (done < bytes) {
        UINT br = 0;
        if (f_read(file, (uint8_t *)buf + done, (UINT)(bytes - done), &br) != FR_OK ||
            br == 0) break;
        done += (size_t)br;
    }
    return done;
}

/* 从当前缓冲取一个字节；缓冲耗尽时从 SD 读下一块。 */
static int get_byte(video_mjpeg_t *r)
{
    if (r->read_pos >= r->read_len) {
        if (!r->opened || r->eof) return -1;
        size_t n = read_full(&r->fil, r->read_buf, VIDEO_MJPEG_READ_BYTES);
        if (!n) {
            r->eof = true;
            return -1;
        }
        r->read_base = r->next_read;
        r->next_read += n;
        r->read_len = n;
        r->read_pos = 0;
    }
    return r->read_buf[r->read_pos++];
}

static uint64_t buffered_offset(const video_mjpeg_t *r)
{
    return r->read_base + r->read_pos;
}

static bool put_byte(video_mjpeg_t *r, uint8_t b)
{
    if (r->frame_len >= VIDEO_MJPEG_FRAME_MAX) return false;
    r->frame_buf[r->frame_len++] = b;
    return true;
}

/* 找到下一个 SOI（FFD8）。文件尾返回 false。 */
static bool find_soi(video_mjpeg_t *r)
{
    int prev = -1;
    for (;;) {
        int c = get_byte(r);
        if (c < 0) return false;
        if (prev == 0xFF && c == 0xD8) return true;
        prev = c;
    }
}

/*
 * 流式读取一帧。进入时 SOI 已经找到；函数把它及后续所有段复制到
 * frame_buf。SOS 后按 JPEG 熵编码规则扫描到 EOI。
 */
static bool parse_frame(video_mjpeg_t *r)
{
    r->frame_len = 0;
    if (!put_byte(r, 0xFF) || !put_byte(r, 0xD8)) return false;

    for (;;) {
        int c = get_byte(r);
        if (c < 0 || c != 0xFF) return false;
        if (!put_byte(r, 0xFF)) return false;

        /* 允许标记前出现多个 FF 填充字节。 */
        do {
            c = get_byte(r);
            if (c < 0) return false;
            if (!put_byte(r, (uint8_t)c)) return false;
        } while (c == 0xFF);

        unsigned marker = (unsigned)c;
        if (marker == 0x00 || marker == 0xD8) return false;
        if (marker == 0xD9) return true; /* 没有 SOS 的空帧，仍按 EOI 结束 */

        /* 无长度的独立标记。 */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;

        int hi = get_byte(r);
        int lo = get_byte(r);
        if (hi < 0 || lo < 0) return false;
        if (!put_byte(r, (uint8_t)hi) || !put_byte(r, (uint8_t)lo)) return false;
        unsigned len = ((unsigned)hi << 8) | (unsigned)lo;
        if (len < 2) return false;

        for (unsigned i = 2; i < len; ++i) {
            c = get_byte(r);
            if (c < 0 || !put_byte(r, (uint8_t)c)) return false;
        }

        if (marker != 0xDA) continue;

        /* SOS 之后是熵编码数据：FF00 是转义，RST0..7 是重启标记，
         * FFD9 是整帧结束；其他标记在 baseline 单扫描帧中视为损坏。 */
        for (;;) {
            c = get_byte(r);
            if (c < 0 || !put_byte(r, (uint8_t)c)) return false;
            if (c != 0xFF) continue;
            int code = get_byte(r);
            if (code < 0 || !put_byte(r, (uint8_t)code)) return false;
            /* 标记前可能还有多个 FF 填充字节；必须一直读到真正的 code，
             * 否则 FFFF D9 会被误当成熵数据而读不到 EOI。 */
            while (code == 0xFF) {
                code = get_byte(r);
                if (code < 0 || !put_byte(r, (uint8_t)code)) return false;
            }
            if (code == 0x00 || (code >= 0xD0 && code <= 0xD7)) continue;
            if (code == 0xD9) return true;
            return false;
        }
    }
}

/*
 * VFS 挂载点不是 FAT 路径的一部分：/sdcard/HEI_MAO_POD_SYS/... 对应
 * FatFs 根目录下的 /HEI_MAO_POD_SYS/...。FF_FS_RPATH=0 时无盘符路径
 * 使用默认卷 0；sd_card 只挂载一个 FAT 卷，且 sd_card.c 的
 * f_getfree(SD_MOUNT_POINT, ...) 也依赖同一个默认卷。
 * 转换失败时直接拒绝，不访问文件系统。
 */
static bool to_fatfs_path(const char *path, char *out, size_t cap)
{
    if (!path) return false;
    const size_t prefix = sizeof(SD_MOUNT_POINT) - 1;
    if (strncmp(path, SD_MOUNT_POINT, prefix) || path[prefix] != '/' ||
        !path[prefix + 1]) return false;
    const char *relative = path + prefix;
    size_t len = strlen(relative);
    if (len >= cap) return false;
    memcpy(out, relative, len + 1);
    return true;
}

/* 取普通文件的真实字节数。FatFs 的 fsize 是 32 位无符号，FAT32 单文件
 * 上限 4 GiB-1，不再有 VFS off_t 的符号回绕问题；目录、空文件和
 * f_stat 失败都视为无效。 */
static bool stat_regular_size(const char *path, uint64_t *size)
{
    FILINFO info;
    if (f_stat(path, &info) != FR_OK || (info.fattrib & AM_DIR) || !info.fsize)
        return false;
    *size = (uint64_t)info.fsize;
    return true;
}

esp_err_t video_mjpeg_open(video_mjpeg_t **out, const char *path)
{
    if (!out || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    char fat_path[VM_PATH_MAX];
    if (!to_fatfs_path(path, fat_path, sizeof(fat_path))) return ESP_ERR_NOT_FOUND;
    uint64_t file_size = 0;
    if (!stat_regular_size(fat_path, &file_size)) return ESP_ERR_NOT_FOUND;

    video_mjpeg_t *r = heap_caps_calloc(1, sizeof(*r),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) return ESP_ERR_NO_MEM;
    r->read_buf = heap_caps_aligned_alloc(VIDEO_MJPEG_IO_ALIGN,
                                          VIDEO_MJPEG_READ_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 对齐分配失败时宁可退回普通 PSRAM（会走慢速逐扇区中转），
     * 也不能让视频完全打不开。 */
    if (!r->read_buf)
        r->read_buf = heap_caps_malloc(VIDEO_MJPEG_READ_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->frame_buf = heap_caps_malloc(VIDEO_MJPEG_FRAME_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->read_buf || !r->frame_buf) {
        video_mjpeg_close(r);
        return ESP_ERR_NO_MEM;
    }
    if (f_open(&r->fil, fat_path, FA_READ) != FR_OK) {
        video_mjpeg_close(r);
        return ESP_ERR_NOT_FOUND;
    }
    r->opened = true;
    r->size = file_size;
    r->read_base = 0;
    r->next_read = 0;
    r->read_len = 0;
    r->read_pos = 0;
    r->eof = false;
    ESP_LOGI(TAG, "read_buf=%p mod512=%u bytes=%u", (void *)r->read_buf,
             (unsigned)((uintptr_t)r->read_buf & (VIDEO_MJPEG_IO_ALIGN - 1U)),
             (unsigned)VIDEO_MJPEG_READ_BYTES);
    *out = r;
    return ESP_OK;
}

void video_mjpeg_detach(video_mjpeg_t *reader)
{
    if (!reader) return;
    if (reader->opened) {
        f_close(&reader->fil);
        reader->opened = false;
    }
    reader->eof = true;
}

void video_mjpeg_close(video_mjpeg_t *reader)
{
    if (!reader) return;
    if (reader->opened) {
        f_close(&reader->fil);
        reader->opened = false;
    }
    heap_caps_free(reader->read_buf);
    heap_caps_free(reader->frame_buf);
    heap_caps_free(reader);
}

bool video_mjpeg_next(video_mjpeg_t *reader, const uint8_t **frame,
                      size_t *bytes, uint64_t *frame_offset)
{
    if (!reader || !frame || !bytes || !frame_offset || !reader->opened) return false;
    if (!find_soi(reader)) return false;

    uint64_t start = buffered_offset(reader) - 2;
    if (!parse_frame(reader)) return false;

    *frame = reader->frame_buf;
    *bytes = reader->frame_len;
    *frame_offset = start;
    return true;
}

bool video_mjpeg_seek(video_mjpeg_t *reader, uint64_t offset)
{
    if (!reader || !reader->opened || offset > reader->size) return false;
    /* 向下对齐到扇区边界，保证后续 f_read 的目标地址和 FATFS 起始偏移
     * 都满足 SDMMC 直接多块 DMA 条件。解析器会从该位置向后找 SOI。 */
    offset &= ~((uint64_t)VIDEO_MJPEG_IO_ALIGN - 1U);
    /* FSIZE_t 是 32 位无符号，FAT32 文件最大 4 GiB-1；size 已受该限制，
     * 这里的检查只防御损坏的 reader 状态。 */
    if (offset > (uint64_t)UINT32_MAX) return false;
    if (f_lseek(&reader->fil, (FSIZE_t)offset) != FR_OK) return false;
    reader->read_base = offset;
    reader->next_read = offset;
    reader->read_len = 0;
    reader->read_pos = 0;
    reader->eof = false;
    return true;
}

uint64_t video_mjpeg_tell(const video_mjpeg_t *reader)
{
    return reader ? buffered_offset(reader) : 0;
}

uint64_t video_mjpeg_size(const video_mjpeg_t *reader)
{
    return reader ? reader->size : 0;
}

bool video_mjpeg_valid(const video_mjpeg_t *reader)
{
    return reader && reader->opened;
}

bool video_mjpeg_at_end(const video_mjpeg_t *reader)
{
    return reader && reader->eof;
}

/*
 * 内存中的帧边界检查。只接受播放器支持的 460x460、8bit、三分量
 * baseline JPEG；444/422/420 都可。它用于缩略图选帧，避免把 APP 段
 * 中内嵌的小缩略图当成完整的视频帧。
 */
static bool frame_end_mem(const uint8_t *d, size_t size, size_t start, size_t *end)
{
    if (start > size || size - start < 4 ||
        d[start] != 0xFF || d[start + 1] != 0xD8) return false;

    size_t p = start + 2;
    bool have_sof = false;
    while (p < size) {
        if (d[p++] != 0xFF) return false;
        while (p < size && d[p] == 0xFF) ++p;
        if (p >= size) return false;
        unsigned marker = d[p++];
        if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7)) return false;
        if (size - p < 2) return false;

        size_t length = be16(d + p);
        if (length < 2 || length > size - p) return false;
        const uint8_t *body = d + p + 2;

        if (marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            /* SOF0 是硬件 JPEG 解码器支持的 baseline；其余 SOF 变体拒绝。 */
            if (marker != 0xC0 || have_sof || length != 17 || body[0] != 8 ||
                be16(body + 1) != VIDEO_MJPEG_HEIGHT ||
                be16(body + 3) != VIDEO_MJPEG_WIDTH ||
                body[5] != 3 ||
                (body[7] != 0x11 && body[7] != 0x21 && body[7] != 0x22) ||
                body[10] != 0x11 || body[13] != 0x11) return false;
            have_sof = true;
        }

        p += length;
        if (marker != 0xDA) continue;
        if (!have_sof || length != 12 || body[0] != 3 ||
            body[7] != 0 || body[8] != 63 || body[9] != 0) return false;

        while (p < size) {
            if (d[p++] != 0xFF) continue;
            while (p < size && d[p] == 0xFF) ++p;
            if (p >= size) return false;
            unsigned code = d[p++];
            if (code == 0x00 || (code >= 0xD0 && code <= 0xD7) ||
                code == 0xFF) continue;
            if (code == 0xD9) {
                *end = p;
                return true;
            }
            return false;
        }
        return false;
    }
    return false;
}

/*
 * 在一个读取窗口内寻找完整帧。窗口内的帧可能被截断，因此如果找到
 * 候选 SOI 但边界检查失败，调用方会从该 SOI 重新读一个完整窗口再试。
 */
static bool find_frame_in_window(const uint8_t *buf, size_t size,
                                 size_t *start, size_t *end)
{
    for (size_t i = 0; i + 1 < size;) {
        if (buf[i] != 0xFF || buf[i + 1] != 0xD8) {
            ++i;
            continue;
        }
        if (frame_end_mem(buf, size, i, end)) {
            *start = i;
            return true;
        }
        ++i;
    }
    return false;
}

esp_err_t video_mjpeg_read_frame_near(const char *path, uint64_t target,
                                      uint8_t *out, size_t out_cap,
                                      size_t *out_len, uint64_t *out_offset)
{
    if (!path || !out || !out_len || !out_offset || out_cap < 4)
        return ESP_ERR_INVALID_ARG;

    char fat_path[VM_PATH_MAX];
    if (!to_fatfs_path(path, fat_path, sizeof(fat_path))) return ESP_ERR_NOT_FOUND;
    uint64_t size = 0;
    if (!stat_regular_size(fat_path, &size)) return ESP_ERR_NOT_FOUND;
    if (target >= size) target = size / 2;
    /* 目标文件偏移也向下对齐；多读的几百字节由 SOI 扫描自然跳过。 */
    target &= ~((uint64_t)VIDEO_MJPEG_IO_ALIGN - 1U);

    /* FIL 自带 FF_MAX_SS（本配置 4 KiB）的扇区缓存，放到视频 worker
     * 16 KiB 栈上会明显抬高最坏栈占用；缩略图只在 worker 内串行使用，
     * 用 PSRAM 堆对象即可，读完后立即释放。 */
    FIL *file = heap_caps_malloc(sizeof(*file), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file) return ESP_ERR_NO_MEM;
    if (f_open(file, fat_path, FA_READ) != FR_OK) {
        heap_caps_free(file);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    /* 先取中间附近；失败后退回文件开头，至少给用户一个首帧预览。
     * 按 64 KiB 分块读取，而不是一次读满 512 KiB：460x460 帧通常只有
     * 几十 KiB，缩短持 SD 锁的时间，避免正在播放的音乐补数据被拖慢。 */
    const uint64_t attempts[2] = {target, 0};
    const size_t chunk = 64U * 1024U;
    for (unsigned attempt = 0; attempt < 2 && result != ESP_OK; ++attempt) {
        uint64_t read_at = attempts[attempt];
        if (read_at >= size || read_at > (uint64_t)UINT32_MAX) continue;
        if (f_lseek(file, (FSIZE_t)read_at) != FR_OK) continue;

        size_t total = 0;
        size_t start = 0, end = 0;
        while (total < out_cap) {
            size_t want = out_cap - total;
            if (want > chunk) want = chunk;
            size_t n = read_full(file, out + total, want);
            if (!n) break;
            total += n;
            if (find_frame_in_window(out, total, &start, &end)) {
                size_t len = end - start;
                if (len > out_cap) break;
                memmove(out, out + start, len);
                *out_len = len;
                *out_offset = read_at + start;
                result = ESP_OK;
                break;
            }
            if (total >= out_cap) break;
        }
    }

    f_close(file);
    heap_caps_free(file);
    return result;
}
