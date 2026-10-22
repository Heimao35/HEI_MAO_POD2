/**
 * @file jpeg_codec.h
 * @brief 全工程共享的硬件 JPEG 解码器。
 *
 * IDF 的 JPEG 驱动允许多个 decoder engine 共享同一个硬件 codec，但 engine
 * 的创建/删除会修改共享的 ISR 处理链表和中断状态，而驱动没有为这些生命周期
 * 操作提供跨 engine 的互斥。封面、锁屏动画和视频曾在不同任务里各自创建/
 * 删除 decoder，正好可能在另一个 engine 解码时释放队列或注册 ISR，造成偶发
 * 的“JPEG 解码器不可用”。
 *
 * 这里只创建一个 decoder engine，所有模块通过 jpeg_codec_process() 串行调用；
 * engine 生命周期内不再删除，避免共享 ISR/queue 的竞态。
 */

#ifndef JPEG_CODEC_H
#define JPEG_CODEC_H

#include "driver/jpeg_decode.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建全局互斥锁。app_main 在使用任何 JPEG 解码前调用一次。 */
esp_err_t jpeg_codec_init(void);

/**
 * @brief 串行解码一张 JPEG。
 *
 * 首次调用时创建共享 decoder engine；创建失败会限速重试。所有调用者共用
 * 同一个 engine，因此 cover、锁屏动画和视频不会同时进入硬件 codec。
 *
 * @param timeout_ms 等待其它解码完成的最长时间
 */
esp_err_t jpeg_codec_process(const jpeg_decode_cfg_t *cfg,
                             const uint8_t *input, uint32_t input_len,
                             uint8_t *output, uint32_t output_cap,
                             uint32_t *output_len,
                             uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_CODEC_H */
