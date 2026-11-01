/* Host test for the chunked bulk reader: every 64 KiB chunk must take and
 * release the SD lock (so the audio worker can interleave), a detach during
 * the read must abort without touching a stale FILE, and a lock timeout must
 * leave a pending handle that the next call closes before opening again. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/sd_bulk_read.c"

/* ---------------- SD lock / detach doubles ---------------- */
static bool locked;
static unsigned lock_calls, unlock_calls, lock_failures;
static unsigned fail_lock_at;   /* 0 = never；否则第 N 次取锁返回失败 */
static unsigned detach_lock_at; /* 0 = never；第 N 次取锁返回前模拟卸载 */
static unsigned detach_calls;
static bool mounted = true;
static void (*detach_cb)(void);

bool sd_card_io_lock(TickType_t timeout)
{
    (void)timeout;
    unsigned n = ++lock_calls;
    if (fail_lock_at && n == fail_lock_at) {
        ++lock_failures;
        return false;
    }
    assert(!locked);
    /* 真实系统的卸载任务必须持有同一把 SD 锁才能调用 detach，因此它只可能
     * 发生在本模块放锁之后、下一次取锁之前。这里在"取锁成功"的时刻模拟
     * 这个已完成的卸载：模块取回锁后应看到失效标志并中止。 */
    if (detach_lock_at && n == detach_lock_at && detach_cb) {
        ++detach_calls;
        detach_cb();
        detach_lock_at = 0;
    }
    locked = true;
    return pdTRUE;
}

void sd_card_io_unlock(void)
{
    assert(locked);
    locked = false;
    ++unlock_calls;
}

bool sd_card_is_mounted(void) { return mounted; }
bool sd_card_is_present(void) { return mounted; }

esp_err_t sd_card_set_bulk_detach_callback(void (*callback)(void))
{
    detach_cb = callback;
    return ESP_OK;
}

/* ---------------- FreeRTOS mutex double ---------------- */
static bool mux_taken;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore;
    (void)ticks;
    assert(!mux_taken);
    mux_taken = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    assert(mux_taken);
    mux_taken = false;
    return pdTRUE;
}

/* ---------------- helpers ---------------- */
static const char *s_path = "sd_bulk_read_test.bin";

static uint8_t expected_byte(size_t i)
{
    return (uint8_t)(i * 31U + 7U);
}

static void make_file(size_t bytes)
{
    FILE *fp = fopen(s_path, "wb");
    assert(fp);
    for (size_t i = 0; i < bytes; ++i) {
        int c = expected_byte(i);
        assert(fputc(c, fp) == c);
    }
    assert(fclose(fp) == 0);
}

static void reset_counters(void)
{
    assert(!locked);
    lock_calls = unlock_calls = lock_failures = 0;
    fail_lock_at = detach_lock_at = detach_calls = 0;
    mounted = true;
}

static void expect_contents(const uint8_t *buf, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        assert(buf[i] == expected_byte(i));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)TAG; /* 主机上 ESP_LOGW 是空宏，TAG 只用于失败日志 */

    assert(sd_bulk_read_init() == ESP_OK);
    assert(detach_cb != NULL);
    /* 重复初始化保持幂等：main 只调用一次，但模块不应因此失效。 */
    assert(sd_bulk_read_init() == ESP_OK);
    puts("PASS init registers the detach callback and is idempotent");

    /* 3 块 + 尾块：应出现 4 次取锁（打开 1 次 + 块间 3 次），而不是 1 次
     * 覆盖整个文件的长临界区。 */
    size_t size = 3U * SD_BULK_READ_CHUNK_BYTES + 123U;
    make_file(size);
    uint8_t *buf = malloc(size);
    assert(buf);

    reset_counters();
    assert(sd_bulk_read_all(s_path, buf, size) == ESP_OK);
    expect_contents(buf, size);
    assert(lock_calls == 4 && unlock_calls == 4 && !locked);
    assert(s_file == NULL);
    puts("PASS 64 KiB chunks take/release the SD lock between blocks");

    /* 单块文件：只应取锁一次。 */
    reset_counters();
    assert(sd_bulk_read_all(s_path, buf, SD_BULK_READ_CHUNK_BYTES) == ESP_OK);
    expect_contents(buf, SD_BULK_READ_CHUNK_BYTES);
    assert(lock_calls == 1 && unlock_calls == 1);
    puts("PASS a single-chunk file uses one short lock section");

    /* 0 字节：只做打开/关闭，不进入读取循环。 */
    reset_counters();
    assert(sd_bulk_read_all(s_path, NULL, 0) == ESP_OK);
    assert(lock_calls == 1 && unlock_calls == 1);
    puts("PASS a zero-size request only opens and closes");

    /* 第 3 次取锁失败（第二块读完之后）：返回超时，句柄保留给下次补关。 */
    reset_counters();
    fail_lock_at = 3;
    assert(sd_bulk_read_all(s_path, buf, size) == ESP_ERR_TIMEOUT);
    assert(!locked);
    assert(s_file != NULL);
    puts("PASS a lock timeout aborts and retains the handle for cleanup");

    /* 下一次调用先补关旧句柄再打开新文件，数据仍然正确。 */
    assert(sd_bulk_read_all(s_path, buf, size) == ESP_OK);
    expect_contents(buf, size);
    assert(s_file == NULL && !locked);
    assert(unlock_calls + lock_failures == lock_calls);
    puts("PASS the next call closes the pending handle before reopening");

    /* 读取中卡被卸载：detach 关闭句柄并置失效标志，本次读取必须中止，
     * 且不能对已失效的 FILE 再读一次。 */
    reset_counters();
    detach_lock_at = 3;
    assert(sd_bulk_read_all(s_path, buf, size) == ESP_ERR_INVALID_STATE);
    assert(detach_calls == 1);
    assert(s_file == NULL && !locked);
    assert(unlock_calls + lock_failures == lock_calls);
    puts("PASS detach during the read aborts without touching a stale FILE");

    /* 打开时卡已卸载：直接失败，不打开文件。 */
    reset_counters();
    mounted = false;
    assert(sd_bulk_read_all(s_path, buf, size) == ESP_ERR_INVALID_STATE);
    assert(s_file == NULL && !locked);
    assert(lock_calls == 1 && unlock_calls == 1);
    puts("PASS an unmounted card fails before opening the file");

    /* 文件不存在：NOT_FOUND，锁仍然配对释放。 */
    reset_counters();
    assert(sd_bulk_read_all("sd_bulk_read_missing.bin", buf, 16) == ESP_ERR_NOT_FOUND);
    assert(s_file == NULL && !locked);
    assert(unlock_calls + lock_failures == lock_calls);
    puts("PASS a missing file reports NOT_FOUND");

    /* 参数检查：未给目标缓冲却要求读取。 */
    assert(sd_bulk_read_all(s_path, NULL, 16) == ESP_ERR_INVALID_ARG);

    free(buf);
    remove(s_path);
    puts("ALL SD BULK READ TESTS PASSED");
    return 0;
}
