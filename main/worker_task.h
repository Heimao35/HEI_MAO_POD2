/**
 * @file worker_task.h
 * @brief 后台 worker 的 PSRAM 栈创建约定（方案 A+，2026-11-14）。
 *
 * ## 为什么使用 PSRAM 栈
 *
 * ESP32-P4 的内部 SRAM 中，256 KiB 被 L2 cache 占用，剩余约 512 KiB 由
 * IRAM、静态 data/bss 和内部堆共享；本工程的主内部堆只有约 282 KiB。
 * 64 KiB PCM ring、I2S DMA、各任务栈和音频解码器都要从这里分配。
 *
 * 旧策略在通用内部堆里反复 xTaskCreate/vTaskDelete 12/16 KiB 任务栈。
 * TLSF 分配器不能搬移正在使用的对象，一个 16 KiB 空闲块被 12 KiB 栈 +
 * 4 KiB 其他对象切开后，即使 12 KiB 栈退出，也无法再拼回 16 KiB 连续块。
 * 结果就是“internal free 还有十几 KiB，largest free block 却只有 9 KiB”，
 * 视频/图片/电子书 worker 偶发创建失败。
 *
 * 因此，本工程把“首次使用创建、之后常驻”的后台 worker 栈统一放到 PSRAM：
 *
 * - TCB 仍由 FreeRTOS 放在内部 RAM（约百余字节），栈在 PSRAM；
 * - 内部堆完全不参与 12/16 KiB 任务栈分配，从根源上消除这类碎片；
 * - 32 MB PSRAM 对几十个 12–16 KiB 栈非常宽裕；
 * - worker 空闲时阻塞在队列/通知上，不占 CPU；首次创建后不再销毁，
 *   因此运行期没有任务栈的分配/释放。
 *
 * ## 使用规则（新增任务时务必遵守）
 *
 * 1. 只有“不访问 NVS/Flash”的后台任务才能用 worker_task_create()。
 *    禁止在任务中调用 nvs_*、esp_partition_*、esp_flash_*、spi_flash_*、
 *    OTA 等会关闭 flash cache（或在 cache 关闭期间访问 PSRAM）的 API。
 *    SD/FatFs、JPEG、LVGL、I2C、USB、ADC 等不属于这一类。
 *
 * 2. 需要访问 NVS/Flash 的任务（system_power、wallpaper、screen_power、
 *    设置保存等）继续使用 xTaskCreatePinnedToCore()，栈留在内部 RAM。
 *
 * 3. 用本 helper 创建的任务，如果将来确实需要退出，必须调用
 *    worker_task_exit()（内部是 vTaskDeleteWithCaps）。绝对不能调用
 *    vTaskDelete(NULL)：该 API 不会释放 PSRAM 栈，会造成永久泄漏。
 *
 * 4. 当前策略是“首次创建后常驻”，所以正常路径不调用 worker_task_exit()；
 *    该函数仅作为未来按需释放方案（例如槽池/临时任务）的入口保留。
 *
 * 5. 参数 stack_bytes 的单位是字节（与现有 VIDEO_TASK_STACK 等宏一致）；
 *    xTaskCreatePinnedToCoreWithCaps 内部按 StackType_t 大小换算。
 *
 * ## 未来扩展建议
 *
 * - 模式类功能优先复用常驻 worker，不要为每个页面新开任务；
 * - 如果某个功能确实需要临时任务，仍用 worker_task_create() 创建，并在
 *   退出时用 worker_task_exit()；更彻底的做法是像模式 worker 一样常驻；
 * - 如果 PSRAM 栈性能不适合某个高实时性任务（例如 60 fps 动画），该任务
 *   应继续使用内部栈，或使用固定大小槽池，不要退回通用内部堆反复申请。
 */
#ifndef WORKER_TASK_H
#define WORKER_TASK_H

#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* 这三个配置是 PSRAM 栈任务的前提：静态任务创建、外部栈合法化、
 * 以及 xTaskCreatePinnedToCoreWithCaps() 本身的存在。配置被改掉时
 * 在编译期直接报错，避免运行时才触发断言/栈非法。 */
#if !CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
#error "worker_task.h requires CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION"
#endif
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "worker_task.h requires CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM"
#endif
#if !CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
#error "worker_task.h requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** worker 栈使用的内存属性：PSRAM、按字节可访问。 */
#define WORKER_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/**
 * @brief 创建一个栈在 PSRAM 的后台 worker。
 *
 * TCB 由 FreeRTOS 放在内部 RAM；栈从 PSRAM 分配。任务必须是常驻的，或者
 * 在退出时调用 worker_task_exit()。详见本文件顶部规则。
 *
 * @param fn          任务入口函数
 * @param name        任务名（FreeRTOS 最多保留 16 字符）
 * @param stack_bytes 栈大小，单位字节（例如 12288、16384）
 * @param arg         传给任务的参数
 * @param prio        优先级
 * @param out         返回任务句柄；失败时置 NULL
 * @param core        绑定的核心；tskNO_AFFINITY 表示不绑定
 * @return pdPASS 成功，其它值失败
 */
static inline BaseType_t worker_task_create(TaskFunction_t fn, const char *name,
                                            uint32_t stack_bytes, void *arg,
                                            UBaseType_t prio, TaskHandle_t *out,
                                            BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio,
                                           out, core, WORKER_TASK_STACK_CAPS);
}

/**
 * @brief 打印 PSRAM 栈 worker 创建失败时的堆余量。
 *
 * 这不是内部任务栈碎片问题：PSRAM 栈创建失败说明 PSRAM 紧张或异常。
 * 同时打印内部堆余量，便于区分“PSRAM 不足”和“内部堆/TLSF 状态”。
 */
static inline void worker_task_log_create_failure(const char *tag, const char *name)
{
    ESP_LOGE(tag,
             "%s worker create failed (PSRAM free %u B, largest %u B; "
             "internal free %u B, largest %u B)",
             name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/**
 * @brief 结束当前 worker_task_create() 创建的任务。
 *
 * 仅供未来确实需要退出/按需释放的任务使用；常驻策略下正常路径不调用。
 * 必须使用 vTaskDeleteWithCaps 而不是 vTaskDelete：普通 vTaskDelete 不会
 * 释放 worker_task_create() 从 PSRAM 静态分配的栈。
 *
 * 注意：该函数不能在中断上下文调用。
 */
static inline void worker_task_exit(void)
{
    vTaskDeleteWithCaps(NULL);
}

#ifdef __cplusplus
}
#endif

#endif /* WORKER_TASK_H */
