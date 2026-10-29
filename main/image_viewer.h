/**
 * @file image_viewer.h
 * @brief 图片查看器后台：扫描 cam/pictures、解码首页预览/网格缩略图/全屏图。
 *
 * 设计边界（与 video_player 一致）：
 * - 所有 SD/FatFs 和 JPEG 解码都在一个后台 worker 中完成；UI 回调由
 *   worker 在 LVGL 锁内同步调用，回调里只更新控件，不再等待 SD/JPEG/LVGL。
 * - 大缓冲在首次使用时一次性从 PSRAM 分配并复用，绝不按图片数量或单张
 *   图片重复分配；文件名表有固定上限，超出时设置 truncated，不能假装
 *   已经列出全部文件。
 * - 只接受 460x460 JPEG（用户按导出说明制作）。尺寸不符、文件过大或
 *   解码失败时回调 ok=false，由 UI 显示占位/错误，绝不伪造图片。
 * - 缩略图请求采用“每槽位最新请求”模型而不是无界队列：快速滚动时旧
 *   请求可以被新 token 丢弃，不会为每个文件创建任务或堆积内存。
 */
#ifndef IMAGE_VIEWER_H
#define IMAGE_VIEWER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMAGE_FOLDER_CAM = 0,       /**< HEI_MAO_POD_SYS/MEDIA/cam（相机） */
    IMAGE_FOLDER_PICTURES = 1,  /**< HEI_MAO_POD_SYS/MEDIA/pictures（其他图片） */
    IMAGE_FOLDER_COUNT
} image_folder_t;

#define IMAGE_NAME_MAX         96U   /**< 文件名上限；更长文件跳过并标记 truncated */
#define IMAGE_INDEX_MAX        4096U /**< 每个文件夹最多索引的文件数 */
#define IMAGE_THUMB_W          100U  /**< 总览网格缩略图宽度 */
#define IMAGE_THUMB_H          100U  /**< 总览网格缩略图高度 */
#define IMAGE_HOME_W           148U  /**< 首页卡片预览宽度 */
#define IMAGE_HOME_H           148U  /**< 首页卡片预览高度 */
#define IMAGE_FULL_W           460U  /**< 全屏图片要求宽度 */
#define IMAGE_FULL_H           460U  /**< 全屏图片要求高度 */
#define IMAGE_FULL_PAD_W       464U  /**< JPEG 解码输出按 16 对齐后的宽度 */
#define IMAGE_FULL_PAD_H       464U  /**< JPEG 解码输出按 16 对齐后的高度 */
#define IMAGE_FULL_BYTES       (IMAGE_FULL_PAD_W * IMAGE_FULL_PAD_H * 2U)
#define IMAGE_MAX_THUMB_SLOTS  32U   /**< UI 网格同时存在的缩略图槽位上限 */
#define IMAGE_JPEG_MAX_BYTES   (2U * 1024U * 1024U) /**< 单张 JPEG 输入上限 */

typedef enum {
    IMAGE_SCAN_OK = 0,   /**< 扫描完成（文件夹不存在视为空目录） */
    IMAGE_SCAN_NO_CARD,  /**< SD 卡未挂载/未插入 */
    IMAGE_SCAN_BUSY,     /**< SD I/O 锁被音乐/视频扫描占用，稍后可重试 */
    IMAGE_SCAN_ERROR,    /**< 目录/SD 读写错误或扫描被拔卡打断 */
} image_scan_result_t;

/** @brief 全屏解码失败原因，供 UI 显示可操作的提示。 */
typedef enum {
    IMAGE_ERROR_NONE = 0,
    IMAGE_ERROR_READ,    /**< 打开/读取失败 */
    IMAGE_ERROR_BUSY,    /**< SD 卡忙/正在重挂载，稍后重试 */
    IMAGE_ERROR_SIZE,    /**< 不是 460x460 */
    IMAGE_ERROR_FORMAT,  /**< JPEG 解码失败（如 progressive/损坏） */
} image_error_t;

/**
 * @brief UI 回调集合。全部由 worker 在 LVGL 锁内调用。
 *
 * 回调中禁止 SD/JPEG/LVGL 锁等待；pixels 指向后台复用的 PSRAM 缓冲，
 * 只在下次同类型请求完成前有效。UI 必须用 token/generation 丢弃过期结果。
 */
typedef struct {
    /** 每个文件夹扫描结果；即使失败也会回调 count=0 以清除旧列表。 */
    void (*library)(image_folder_t folder, uint32_t count, bool truncated,
                    uint32_t generation, void *ctx);
    /** 首页第一张图片缩略图；ok=false 时保持占位图。 */
    void (*home_preview)(image_folder_t folder, const uint8_t *pixels,
                         uint16_t w, uint16_t h, uint32_t generation,
                         bool ok, void *ctx);
    /** 两个文件夹都处理完后调用一次，用于结束加载提示和错误提示。 */
    void (*scan_done)(image_scan_result_t result, uint32_t generation, void *ctx);
    /** 网格缩略图；slot 由 UI 指定，token 用于丢弃过期解码。 */
    void (*thumbnail)(uint32_t slot, const uint8_t *pixels, uint16_t w,
                      uint16_t h, uint32_t token, bool ok, void *ctx);
    /** 全屏图；pixels 为 464x464 RGB565，页面裁剪右侧/下侧 4 像素。 */
    void (*full_image)(const uint8_t *pixels, uint16_t w, uint16_t h,
                       image_folder_t folder, uint32_t index, const char *name,
                       uint32_t token, bool ok, image_error_t error, void *ctx);
} image_viewer_ops_t;

/** @brief 创建 worker 与请求锁（不分配图片大缓冲，首次扫描时才分配）。 */
esp_err_t image_viewer_init(void);

/** @brief 注册 UI 回调；可在 init 之前调用，只保存指针。 */
void image_viewer_set_ops(const image_viewer_ops_t *ops, void *ctx);

/** @brief 请求重新扫描两个文件夹；重复请求会合并。 */
void image_viewer_request_scan(void);

/**
 * @brief 请求一个网格缩略图。
 * @param slot  0..IMAGE_MAX_THUMB_SLOTS-1
 * @param token UI 每次更新槽位内容时递增；后台只发布最新 token。
 */
void image_viewer_request_thumbnail(image_folder_t folder, uint32_t index,
                                    uint32_t slot, uint32_t token);

/** @brief 请求全屏图片；同一时刻只应有一个 UI 请求在途。 */
void image_viewer_request_full(image_folder_t folder, uint32_t index,
                               uint32_t token);

/** @brief 取消在途全屏解码（返回总览/锁屏）；迟到结果会被丢弃。 */
void image_viewer_cancel_full(void);

/** @brief 取消所有在途缩略图（离开总览/锁屏）；迟到结果会被丢弃。 */
void image_viewer_clear_thumbs(void);

/**
 * @brief 告知后台是否处于全屏查看状态。
 *
 * 全屏时 worker 会在当前图片发布后预取上一张/下一张到两块独立的全屏
 * 缓存；退出全屏后停止预取并释放缓存占用，避免总览/其他页面继续读取
 * SD。该调用只写原子标志，不等待 SD/JPEG。
 */
void image_viewer_set_full_active(bool active);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_VIEWER_H */
