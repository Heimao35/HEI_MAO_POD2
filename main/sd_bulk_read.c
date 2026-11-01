/**
 * @file sd_bulk_read.c
 * @brief 大文件分块读取实现：每 64 KiB 取放一次 SD 锁，块间让音频插队。
 *
 * 读取顺序（全部在 worker 任务里执行，不持有 LVGL 锁）：
 *   取 SD 锁 -> 打开文件 -> 读一块 -> 放锁 -> 取回锁 -> 读下一块 ...
 * 放锁的瞬间，正卡在 sd_card_io_lock() 上的音频 worker（core0 优先级 6，
 * 高于本模块的两个调用方）会直接被唤醒拿到锁补 PCM，然后本模块再取回
 * 锁继续读。持锁时间由"整个文件"缩短为"一块"。
 *
 * FILE 句柄生命周期与 audio_output 保持一致：
 * - 文件句柄只在持有 SD 锁时访问；
 * - SD 卸载路径（sd_card.c do_unmount）在同一个锁内调用 detach 回调，
 *   关闭句柄并置 s_detached；
 * - 每块取回锁后先检查 s_detached，已失效就中止，绝不继续 fread。
 */
#include "sd_bulk_read.h"

#include "sd_card.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "sd_bulk";

/* 与 ebook/image_viewer 原来的整文件持锁上限一致：卡或其它长任务异常时
 * 最终失败而不是无限等待。正常情况下块间锁最多被占几十毫秒。 */
#define SD_BULK_LOCK_MS 5000U

/* 串行化电子书和图片两个 worker：模块只有一个 FILE 和一个卸载回调槽，
 * 两个任务同时读会互相踩句柄。两者本来就只出现在各自的模式里，
 * 串行化的代价最多是等待一次完整读取。 */
static SemaphoreHandle_t s_mux;

/* 以下状态只在持有 SD 锁时访问（打开/读取/关闭），与 detach 回调互斥。 */
static FILE *s_file;

/* 卸载回调已经关闭 s_file 并置位；分块之间取回锁后读到就放弃本次读取。 */
static atomic_bool s_detached;

/* SD 卸载路径在持 SD 锁时调用：只允许关闭文件、发布标志。 */
static void bulk_detach(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    atomic_store(&s_detached, true);
}

esp_err_t sd_bulk_read_init(void)
{
    if (!s_mux) {
        SemaphoreHandle_t mux = xSemaphoreCreateMutex();
        if (!mux) return ESP_ERR_NO_MEM;
        s_mux = mux;
    }
    return sd_card_set_bulk_detach_callback(bulk_detach);
}

/* 在持有 SD 锁时关闭文件。fclose 失败也把指针清空：句柄状态已不可信，
 * 继续保留只会让下一次打开撞上旧句柄。 */
static bool close_while_locked(void)
{
    if (!s_file) return true;
    int rc = fclose(s_file);
    s_file = NULL;
    return rc == 0;
}

esp_err_t sd_bulk_read_all(const char *path, uint8_t *dst, size_t size)
{
    if (!s_mux || !path || (!dst && size)) return ESP_ERR_INVALID_ARG;

    if (!xSemaphoreTake(s_mux, portMAX_DELAY)) return ESP_ERR_TIMEOUT;

    esp_err_t result = ESP_OK;

    /* 上一次块间取锁超时可能留下未关闭的句柄；先补关再打开新文件。 */
    if (s_file) {
        if (!sd_card_io_lock(pdMS_TO_TICKS(SD_BULK_LOCK_MS))) {
            xSemaphoreGive(s_mux);
            return ESP_ERR_TIMEOUT;
        }
        bool closed = close_while_locked();
        sd_card_io_unlock();
        if (!closed) {
            xSemaphoreGive(s_mux);
            return ESP_FAIL;
        }
    }

    /* 打开文件本身也要读 FAT 目录扇区，同样只持短锁。 */
    if (!sd_card_io_lock(pdMS_TO_TICKS(SD_BULK_LOCK_MS))) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_TIMEOUT;
    }
    /* 复位失效标志必须在锁内：detach 回调同样持锁执行，这样它不会在
     * "复位标志"和"打开文件"之间插进来，造成开关卡时的误判。 */
    atomic_store(&s_detached, false);
    bool locked = true;
    if (!sd_card_is_mounted() || !sd_card_is_present()) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        s_file = fopen(path, "rb");
        if (!s_file) result = ESP_ERR_NOT_FOUND;
    }

    size_t done = 0;
    while (result == ESP_OK && done < size) {
        size_t want = size - done;
        if (want > SD_BULK_READ_CHUNK_BYTES) want = SD_BULK_READ_CHUNK_BYTES;
        size_t got = fread(dst + done, 1, want, s_file);
        if (got != want) {
            result = ESP_FAIL;
            break;
        }
        done += got;
        if (done >= size) break;

        /* 关键一步：放锁给音频 worker 补 PCM，再重新取锁读下一块。
         * FreeRTOS 在 xSemaphoreGive 时优先唤醒正在等锁的高优先级任务，
         * 所以音频通常在本任务重新取锁之前就完成了一次 16 KiB 读取。 */
        sd_card_io_unlock();
        locked = false;
        if (!sd_card_io_lock(pdMS_TO_TICKS(SD_BULK_LOCK_MS))) {
            /* 句柄仍开着，下一次调用开头会补关；绝不能在这里无锁访问。 */
            result = ESP_ERR_TIMEOUT;
            break;
        }
        locked = true;
        if (atomic_load(&s_detached)) {
            /* 卸载回调已经在持锁状态下关闭并置空 s_file，这里不能再碰它。 */
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }

    if (locked) {
        if (!close_while_locked() && result == ESP_OK) result = ESP_FAIL;
        sd_card_io_unlock();
    }
    xSemaphoreGive(s_mux);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Read %s failed: %s (%u/%u bytes)", path,
                 esp_err_to_name(result), (unsigned)done, (unsigned)size);
    }
    return result;
}
