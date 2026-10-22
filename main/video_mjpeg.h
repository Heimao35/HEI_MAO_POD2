/**
 * @file video_mjpeg.h
 * @brief 裸 MJPEG（连续 JPEG 帧）文件读取与帧边界解析。
 *
 * 视频目录只接受 460x460、8bit、三分量 baseline JPEG 的裸串流，不含音频。
 * 解析器按 JPEG 段长度跳过 APP/DQT/SOF/DHT 等段，再在 SOS 熵编码区识别
 * FFD9，不能直接搜索第一个 FFD9：APP 段里的缩略图也可能含 FFD9。
 *
 * 所有 f_open/f_read/f_lseek/f_close 都由调用方持有 sd_card_io_lock()。
 * 播放线程长期持有一个 video_mjpeg_t；缩略图使用
 * video_mjpeg_read_frame_near() 短时打开文件，读完立即关闭。这样 SD
 * 卸载时只需要关闭播放线程的文件对象。
 *
 * 文件 I/O 直接使用 FatFs（f_open/f_read/f_lseek/f_close），不经过 VFS：
 * ESP-IDF VFS 的 off_t 是 32 位有符号，只能定位到 2 GiB；FatFs 的 FSIZE_t
 * 是 32 位无符号，可完整支持 FAT32 单文件上限 4 GiB-1，进度条能跳转到
 * 文件任意位置。路径在模块内去掉 VFS 挂载点前缀并使用默认卷。
 */

#ifndef VIDEO_MJPEG_H
#define VIDEO_MJPEG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从视频文件名识别目标帧率。
 *
 * 裸 MJPEG 没有时间戳/帧率字段，约定文件名包含 24fps、30fps、60fps
 * （不区分大小写）即可，例如 clip_24fps.mjpeg。未标记返回 0，由播放器
 * 使用默认 60fps 以兼容已有文件。
 */
uint8_t video_mjpeg_fps_from_name(const char *name);

#define VIDEO_MJPEG_WIDTH       460U
#define VIDEO_MJPEG_HEIGHT      460U
/* 64 KiB 能覆盖约 2~3 帧；单次阻塞比 256 KiB 短，60fps 节拍更容易守住。
 * 读缓冲必须按 VIDEO_MJPEG_IO_ALIGN 对齐，长度也是 512 的倍数。 */
#define VIDEO_MJPEG_READ_BYTES  (64U * 1024U)
#define VIDEO_MJPEG_FRAME_MAX   (1024U * 1024U)
/* v5.5.4 sdmmc_read_sectors() 只在目标地址和长度满足缓存对齐时才直接
 * DMA 多块读取；否则退回每 512B 一次临时 DMA，视频吞吐会从数 MB/s
 * 掉到约 0.5 MB/s。读缓冲按 SD 扇区对齐，文件偏移也向下对齐。 */
#define VIDEO_MJPEG_IO_ALIGN    512U

typedef struct video_mjpeg video_mjpeg_t;

/**
 * @brief 打开裸 MJPEG 文件并分配读缓冲/帧缓冲（PSRAM）。
 * @note 调用方必须持有 SD 锁；成功后文件保持打开，直到 next/close/detach。
 */
esp_err_t video_mjpeg_open(video_mjpeg_t **out, const char *path);

/** @brief 关闭文件并释放全部缓冲。调用方必须持有 SD 锁。 */
void video_mjpeg_close(video_mjpeg_t *reader);

/**
 * @brief SD 卸载回调专用：只关闭 FIL，不取 SD 锁、不释放缓冲。
 * @note 调用方（sd_card）已经持有 SD 锁，且随后会卸载 VFS。
 */
void video_mjpeg_detach(video_mjpeg_t *reader);

/**
 * @brief 读取下一帧完整 JPEG。
 * @param[out] frame 指向内部帧缓冲，下一次调用前保持有效
 * @param[out] bytes 帧长度
 * @param[out] frame_offset 该帧在文件中的起始偏移
 * @return true 成功；false 表示 EOF、损坏或超过帧上限
 */
bool video_mjpeg_next(video_mjpeg_t *reader, const uint8_t **frame,
                      size_t *bytes, uint64_t *frame_offset);

/**
 * @brief 跳到文件偏移。下一帧会从该位置之后的下一个 SOI 开始。
 * @note  受 FatFs FSIZE_t 与 FAT32 限制，可定位到 4 GiB-1 以内的任意位置。
 *        跳转按 512 字节向下对齐；失败时返回 false，原有文件位置不变。
 */
bool video_mjpeg_seek(video_mjpeg_t *reader, uint64_t offset);

/** @brief 当前逻辑文件偏移（已消费到解析器缓冲中的位置）。 */
uint64_t video_mjpeg_tell(const video_mjpeg_t *reader);

/** @brief 文件总字节数。 */
uint64_t video_mjpeg_size(const video_mjpeg_t *reader);

/** @brief 文件句柄是否仍然有效（SD 拔出后由 detach 清零）。 */
bool video_mjpeg_valid(const video_mjpeg_t *reader);

/** @brief 上一次 next 是否因为到达文件末尾而失败。 */
bool video_mjpeg_at_end(const video_mjpeg_t *reader);

/**
 * @brief 从 target 附近取出一帧完整的 460x460 MJPEG 帧（缩略图用）。
 *
 * 先 seek 到 target，向后寻找下一个 SOI 并解析；若 target 附近没有完整帧，
 * 自动回退到文件开头取第一帧。out 同时作为读取窗口和返回帧缓冲，成功时
 * 帧被 memmove 到 out 开头。调用方必须持有 SD 锁；函数内部会打开并关闭
 * 文件，不会留下长期 FILE。
 *
 * @param[in]  path      文件路径
 * @param[in]  target    期望的中间位置（通常 file_size/2）
 * @param[out] out       输出缓冲，同时作为读取窗口
 * @param[in]  out_cap   缓冲容量（建议 >= VIDEO_MJPEG_FRAME_MAX）
 * @param[out] out_len   返回帧长度
 * @param[out] out_offset 返回帧在文件中的偏移
 */
esp_err_t video_mjpeg_read_frame_near(const char *path, uint64_t target,
                                      uint8_t *out, size_t out_cap,
                                      size_t *out_len, uint64_t *out_offset);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_MJPEG_H */
