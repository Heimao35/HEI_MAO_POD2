/**
 * @file sd_bulk_read.h
 * @brief 大文件分块读取：块间释放 SD 锁，避免长临界区让音频欠载。
 *
 * 使用场景是"一次要把整个文件读进内存"的路径（电子书 TXT、图片查看器
 * 的 JPEG）。原来的做法是在一个 sd_card_io_lock() 临界区里一次 fread
 * 读完整个文件：
 *   - 本板 SD 20 MHz 4-bit 原始顺序读约 8.7 MB/s（见 sd_card.c 的注释），
 *     一本几 MB 的书持锁可达数百毫秒；
 *   - 音频 worker 只用 5 ms 等锁，拿不到就退避重试，期间内部 64 KiB
 *     PCM 环被 DMA 唱空并按欠载补零，听感就是"音乐暂停了一下"。
 *
 * 本模块把读取拆成 64 KiB 一块：每块取一次锁、读一块、立刻放锁。音频
 * worker 在放锁瞬间就能拿到锁补数据，长读取不再造成 PCM 欠载。
 *
 * 线程与句柄约定：
 * - 调用者不得持有 SD 锁，也不得在 LVGL 锁内调用；
 * - 模块内部串行化（同一时刻只服务一个 worker）；FILE 可以跨放锁存活，
 *   但 SD 卸载路径会在持锁状态下关闭它并置失效标志，读取随即中止。
 *   这与 audio_output 的 detach 约定一致，绝不使用已失效的 FILE。
 */
#ifndef SD_BULK_READ_H
#define SD_BULK_READ_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单块字节数。本板 20 MHz 4-bit SD 顺序读约 8.7 MB/s，64 KiB 约 8-15 ms
 * 持锁（含 FATFS 目录/簇处理），远小于 64 KiB PCM 环在 48 kHz 下的约
 * 165 ms 余量；再小只会增加取锁次数，收益有限。 */
#define SD_BULK_READ_CHUNK_BYTES (64U * 1024U)

/** @brief 创建串行化互斥量并注册 SD 卸载回调；main 在 sd_card_init 之后调用一次。 */
esp_err_t sd_bulk_read_init(void);

/**
 * @brief 分块读取整个文件到 dst。
 *
 * @param path  文件路径（VFS 路径）。
 * @param dst   目标缓冲，至少 size 字节；size 为 0 时可为 NULL。
 * @param size  要读取的字节数（调用方已 stat 得到）。
 *
 * 块间释放 SD 锁；任何一块取锁超时或读取中卡被卸载都会中止并返回错误。
 * @return ESP_OK；ESP_ERR_INVALID_STATE 未初始化/卡已卸载；
 *         ESP_ERR_TIMEOUT 取 SD 锁超时；ESP_ERR_NOT_FOUND 文件打不开；
 *         ESP_FAIL 短读或关闭失败。
 */
esp_err_t sd_bulk_read_all(const char *path, uint8_t *dst, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SD_BULK_READ_H */
