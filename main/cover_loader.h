#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define COVER_WIDTH 240
#define COVER_HEIGHT 240
#define COVER_JPEG_MAX_BYTES (256U * 1024U)
#define COVER_CACHE_SLOTS 4

/* 初始化在注册 UI 后端前完成；只有媒体 worker 可以 load / destroy。
 * acquire 命中和 load 成功均返回一个持有引用；调用者必须 release。
 * UI 如需在调用结束后继续使用像素，须单独 retain，动画旧图也必须持有。
 * 查询/引用计数由短临界区保护，临界区内不分配内存、不访问 SD、不解码。 */
esp_err_t cover_loader_init(void);
const uint8_t *cover_loader_acquire(uint32_t generation, uint32_t index);
esp_err_t cover_loader_load(const char *path, uint32_t generation, uint32_t index,
                            const uint8_t **pixels);
void cover_loader_retain(const uint8_t *pixels);
void cover_loader_release(const uint8_t *pixels);
/* 重扫/拔卡只废弃缓存键，不破坏仍由 UI 持有的像素。 */
void cover_loader_invalidate(void);
void cover_loader_destroy(void); /* UI 和 worker 均停止使用后调用。 */
