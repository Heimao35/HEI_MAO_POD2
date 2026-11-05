/**
 * @file wallpaper.h
 * @brief 用户自定义壁纸：Flash 持久化、PSRAM 显示缓冲和页面 canvas 挂接。
 *
 * 设计边界（参考 lock_animation.c）：
 * - 壁纸只保存**解码后的 464x464 RGB565 像素**，不做 JPEG 重新解码。
 *   用户只能在图片查看器支持的 460x460 图片里选择，因此不存在缩放；
 *   464 是 JPEG 硬件解码按 16 对齐后的尺寸，页面只有 460x460，右/下
 *   各 4 像素由父对象裁掉（与锁屏动画/视频播放器的 canvas 做法一致）。
 * - 原始像素按两个槽位（A/B）写入独立 Flash 分区：先写非活动槽并读回
 *   CRC，最后才写含 CRC 的 64 字节头部作为提交点；旧槽全程保持原样。
 *   掉电或异常只会在下次启动被忽略，不会把半张图当成有效壁纸。
 * - 启动时只读 Flash，不访问 SD；因此没有 SD 卡也能保留壁纸。
 * - 显示使用两块 PSRAM 缓冲（front/back）。后台保存成功后，在 LVGL
 *   锁内交换指针并让所有已注册 canvas 指向新缓冲，不出现半帧画面。
 *
 * 线程约定：
 * - wallpaper_init() 在 app_main 中、UI 创建之前调用，不需要 LVGL 锁；
 * - wallpaper_create_canvas() 必须在 LVGL 锁内调用（页面创建期间）；
 * - wallpaper_commit() 可在 LVGL 回调中调用：它只做一次 PSRAM 拷贝并
 *   创建一个一次性 worker，Flash 擦写完全在 worker 内完成；
 * - 保存结果通过 wallpaper_status() 轮询，UI 用定时器发布；不在 worker
 *   中回调 UI，避免 UI 页面已关闭后还访问对象。
 */
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 壁纸原图与解码缓冲尺寸。460 是 UI 要求；464 是 JPEG 解码器对 460
 * 按 16 对齐后的输出宽度/高度，页面会裁掉多出的 4 像素。 */
#define WALLPAPER_WIDTH      460U
#define WALLPAPER_HEIGHT     460U
#define WALLPAPER_PAD_WIDTH  464U
#define WALLPAPER_PAD_HEIGHT 464U
#define WALLPAPER_PIXEL_BYTES \
    ((size_t)WALLPAPER_PAD_WIDTH * WALLPAPER_PAD_HEIGHT * 2U)

/** @brief 壁纸保存状态；UI 通过轮询而不是后台回调获取。 */
typedef enum {
    WALLPAPER_STATE_IDLE = 0, /**< 没有进行中的保存，也没有新结果 */
    WALLPAPER_STATE_SAVING,   /**< 后台正在擦写 Flash */
    WALLPAPER_STATE_SAVED,    /**< 最近一次保存成功，画面已切换 */
    WALLPAPER_STATE_ERROR,    /**< 最近一次保存失败，旧壁纸保持不变 */
} wallpaper_state_t;

/**
 * @brief 加载 Flash 中的壁纸到 PSRAM（不访问 SD/LVGL）。
 *
 * 分区不存在（尚未烧录新分区表）或没有任何有效槽位时返回
 * ESP_ERR_NOT_FOUND，但后续 UI 仍可正常创建 canvas；只是不显示壁纸，
 * wallpaper_commit() 也会明确失败。调用一次即可。
 */
esp_err_t wallpaper_init(void);

/** @brief 当前是否有可显示的壁纸（启动加载或保存成功后为 true）。 */
bool wallpaper_available(void);

/**
 * @brief 在页面对象的最底层创建一个全屏壁纸 canvas。
 *
 * 必须在该页面的其他子对象之前调用，canvas 才会位于内容下方；页面
 * 460x460 会裁掉 canvas 右侧/下侧各 4 像素。没有壁纸时 canvas 隐藏，
 * 页面保持原来的纯色背景。调用方必须持有 LVGL 锁。
 *
 * @return 新建的 canvas；失败返回 NULL（不影响页面其余内容）。
 */
lv_obj_t *wallpaper_create_canvas(lv_obj_t *parent);

/**
 * @brief 请求把一张 464x464 RGB565 图设为壁纸。
 *
 * 只复制像素并启动一次性 worker；Flash 擦写在 worker 中进行，不会
 * 阻塞 LVGL 回调。同一时间只允许一个保存请求。调用方必须持有 LVGL
 * 锁（仅用于保证像素来源在复制期间不被其他 UI 操作释放，函数本身
 * 不访问 LVGL 对象）。
 *
 * @return ESP_OK 已接受；ESP_ERR_INVALID_STATE 存储不可用或正在保存；
 *         ESP_ERR_NO_MEM 无法分配后台缓冲。
 */
esp_err_t wallpaper_commit(const uint8_t *pixels);

/** @brief 查询保存状态；WALLPAPER_STATE_SAVED/ERROR 需要 UI 用 ack 清除。 */
wallpaper_state_t wallpaper_status(void);

/** @brief 清除 SAVED/ERROR 结果，避免 UI 下次打开时重复显示旧提示。 */
void wallpaper_ack_result(void);

#ifdef __cplusplus
}
#endif

#endif /* WALLPAPER_H */
