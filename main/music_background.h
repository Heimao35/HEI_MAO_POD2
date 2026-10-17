#pragma once
#include <stdint.h>
#include "esp_err.h"

#define MUSIC_BACKGROUND_WIDTH 460
#define MUSIC_BACKGROUND_HEIGHT 460
#define MUSIC_BACKGROUND_SLOTS 3
/* 80×80 上做三次可分离盒式模糊，近似柔和的高斯模糊。
 * 白色混合 38/255≈15%，在生成时烘焙，不增加 UI 每帧的遮罩混合。 */
#define MUSIC_BACKGROUND_SMALL_SIDE 80
#define MUSIC_BACKGROUND_BLUR_RADIUS 4
#define MUSIC_BACKGROUND_BLUR_PASSES 3
#define MUSIC_BACKGROUND_WHITE_OPA 38U

/* 初始化在绑定 UI 后端前完成；只有媒体 worker 可 generate / destroy。
 * generate 借用一张已持有的 240×240 原生 RGB565 封面，不修改它。
 * acquire / generate 成功返回持有引用；显示对象需 retain，持有者须 release。
 * 缓存只在短临界区内管理键/引用，不在锁内处理图片或分配内存。 */
esp_err_t music_background_init(void);
const uint8_t *music_background_acquire(uint32_t generation, uint32_t index);
esp_err_t music_background_generate(const uint8_t *cover, uint32_t generation,
                                     uint32_t index, const uint8_t **pixels);
void music_background_retain(const uint8_t *pixels);
void music_background_release(const uint8_t *pixels);
void music_background_invalidate(void);
void music_background_destroy(void); /* 所有 UI / worker 引用释放后调用。 */
