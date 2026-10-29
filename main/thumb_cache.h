/**
 * @file thumb_cache.h
 * @brief 缩略图磁盘缓存：把已解码的 RGB565 小图写入 SD 的 .hmcache。
 *
 * 目的：进入图片总览/视频列表时避免重新读取整张 460x460 JPEG 并做全尺寸
 * 解码，改为读取一份小图。缓存完全可丢弃：文件缺失、损坏或源文件
 * 大小/修改时间变化时自动按未命中处理，不影响原文件。
 *
 * 设计：
 * - 每种缩略图有固定数量的槽文件，文件名固定，因此缓存总量有上限，
 *   不需要扫描/清理整个缓存目录。
 * - slot = hash(源路径 + 文件大小 + 修改时间) % 槽数。不同图片映射到
 *   同一槽时后者直接覆盖前者，前者下次变成未命中，不影响正确性。
 * - 文件头记录 magic/版本/类型/尺寸/格式/源大小/源时间/键/CRC；读取时
 *   全部校验，任何不一致都返回 false。
 * - 调用者不能持有 sd_card_io_lock：本模块内部用短超时获取，SD 争用或
 *   卡未挂载时静默跳过。缓存是可选优化，绝不阻塞 UI/SD 主流程。
 */
#ifndef THUMB_CACHE_H
#define THUMB_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 缩略图类型；每种类型有独立的槽目录和固定尺寸。 */
typedef enum {
    THUMB_CACHE_IMG = 0,   /**< 100x100 图片总览缩略图 */
    THUMB_CACHE_HOME,      /**< 148x148 图片首页预览 */
    THUMB_CACHE_VIDEO,     /**< 176x176 视频列表缩略图 */
    THUMB_CACHE_KIND_COUNT
} thumb_cache_kind_t;

/**
 * @brief 尝试读取一张缓存缩略图。
 *
 * @param kind        缩略图类型（决定槽目录/槽数量）
 * @param source_path 原始图片或视频的 VFS 路径；用于与源文件 stat 生成键
 * @param width       期望宽度，必须与该类型一致
 * @param height      期望高度，必须与该类型一致
 * @param pixels      输出 RGB565 缓冲，大小 = width*height*2
 * @return true 表示命中且像素/CRC 校验通过；false 表示未命中或缓存不可用
 */
bool thumb_cache_load(thumb_cache_kind_t kind, const char *source_path,
                      uint16_t width, uint16_t height, uint8_t *pixels);

/**
 * @brief 把一张已解码的缩略图写入缓存（尽力而为）。
 *
 * 写入失败、SD 忙或源文件 stat 失败时直接放弃；不重试、不阻塞调用者。
 *
 * @param kind        缩略图类型
 * @param source_path 原始图片或视频的 VFS 路径
 * @param width       缩略图宽度
 * @param height      缩略图高度
 * @param pixels      RGB565 像素，大小 = width*height*2
 */
void thumb_cache_store(thumb_cache_kind_t kind, const char *source_path,
                       uint16_t width, uint16_t height, const uint8_t *pixels);

#ifdef __cplusplus
}
#endif

#endif /* THUMB_CACHE_H */
