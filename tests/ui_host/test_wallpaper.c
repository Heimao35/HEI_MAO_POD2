/* 生产壁纸存储：双槽 A/B 写入、CRC 校验、掉电/写失败回退和重启加载。
 * 用 NOR 语义的 RAM 分区替身，不访问真实 Flash、LVGL 或 SD 卡。 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 媒体 host 的 FreeRTOS 替身只声明测试用到的任务接口；生产 wallpaper.c
 * 在 worker 末尾调用 vTaskDelete(NULL)，这里补上声明避免隐式声明。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void vTaskDelete(TaskHandle_t handle);
#include "../../main/wallpaper.c"

static uint8_t flash[2 * SLOT_BYTES];
static esp_partition_t partition = {.size = sizeof(flash), .erase_size = SECTOR_BYTES};
static bool partition_present = true;
static unsigned reads, writes, erases;
static int fail_write, fail_erase;
static size_t live;

typedef union { size_t size; long double align; } allocation_t;
void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocation_t *a = malloc(sizeof(*a) + bytes);
    assert(a);
    a->size = bytes;
    live += bytes;
    return a + 1;
}
void heap_caps_free(void *p)
{
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    live -= a->size;
    free(a);
}
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, uint32_t bytes)
{
    crc = ~crc;
    while (bytes--) {
        crc ^= *data++;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
    }
    return ~crc;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA &&
           subtype == ESP_PARTITION_SUBTYPE_ANY &&
           !strcmp(label, PARTITION_LABEL));
    return partition_present ? &partition : NULL;
}
static void bounds(const esp_partition_t *p, size_t offset, size_t bytes)
{
    assert(p == &partition && offset <= p->size && bytes <= p->size - offset);
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *data, size_t bytes)
{
    bounds(p, offset, bytes);
    ++reads;
    memcpy(data, flash + offset, bytes);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t offset, const void *data, size_t bytes)
{
    bounds(p, offset, bytes);
    ++writes;
    bool fail = fail_write && writes == (unsigned)fail_write;
    size_t n = fail ? bytes / 2 : bytes;
    const uint8_t *src = data;
    for (size_t i = 0; i < n; ++i) {
        /* NOR 只能把 1 写成 0；写入前必须已经擦除。 */
        assert((flash[offset + i] & src[i]) == src[i]);
        flash[offset + i] &= src[i];
    }
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t bytes)
{
    bounds(p, offset, bytes);
    assert(!(offset % SECTOR_BYTES) && !(bytes % SECTOR_BYTES));
    ++erases;
    bool fail = fail_erase && erases == (unsigned)fail_erase;
    memset(flash + offset, 0xFF, fail ? bytes / 2 : bytes);
    return fail ? ESP_FAIL : ESP_OK;
}

/* 一次性 worker：保存到变量后由测试同步执行，避免真实线程时序。 */
typedef void (*wallpaper_task_fn)(void *);
static wallpaper_task_fn pending_task;
static unsigned task_creates;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    assert(!strcmp(name, "wallpaper") && stack == 6144 && priority == 3 && !arg);
    pending_task = fn;
    ++task_creates;
    if (handle) *handle = NULL;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t handle) { (void)handle; }

/* worker 发布画面时使用的 LVGL 锁；本测试没有注册 canvas，锁只计数。 */
static unsigned lock_calls;
bool lvgl_port_lock(uint32_t timeout)
{
    assert(timeout == 500);
    ++lock_calls;
    return true;
}
void lvgl_port_unlock(void) {}

static void run_worker(void)
{
    assert(pending_task);
    wallpaper_task_fn fn = pending_task;
    pending_task = NULL;
    fn(NULL);
}

static void boot(void)
{
    heap_caps_free(s_front);
    heap_caps_free(s_back);
    heap_caps_free(s_scratch);
    s_front = s_back = s_scratch = NULL;
    s_loaded = false;
    s_part = NULL;
    s_initialized = false;
    s_canvas_count = 0;
    atomic_store(&s_saving, false);
    atomic_store(&s_state, WALLPAPER_STATE_IDLE);
    pending_task = NULL;
    reads = writes = erases = 0;
    fail_write = fail_erase = 0;
    lock_calls = 0;
}

static uint8_t image_a[WALLPAPER_PIXEL_BYTES];
static uint8_t image_b[WALLPAPER_PIXEL_BYTES];
static uint8_t image_c[WALLPAPER_PIXEL_BYTES];

static bool slot_valid(unsigned index)
{
    slot_t slot;
    return read_header(s_part, index, &slot);
}
static size_t slot_payload(unsigned index)
{
    return (size_t)index * SLOT_BYTES + SECTOR_BYTES;
}

int main(void)
{
    memset(image_a, 0x11, sizeof(image_a));
    memset(image_b, 0x22, sizeof(image_b));
    memset(image_c, 0x33, sizeof(image_c));
    memset(flash, 0xFF, sizeof(flash));

    boot();
    partition_present = false;
    assert(wallpaper_init() == ESP_ERR_NOT_FOUND);
    assert(wallpaper_commit(image_a) == ESP_ERR_INVALID_STATE);
    puts("PASS missing partition disables init/commit without touching flash");

    /* 首次保存：槽 0、序号 1，worker 完成后 front 指向新图。 */
    boot();
    partition_present = true;
    assert(wallpaper_init() == ESP_ERR_NOT_FOUND);
    assert(!wallpaper_available());
    assert(wallpaper_commit(image_a) == ESP_OK);
    assert(task_creates == 1 &&
           atomic_load(&s_state) == WALLPAPER_STATE_SAVING);
    /* 后台未完成时第二次提交必须被拒绝，不排队第二份擦写。 */
    assert(wallpaper_commit(image_b) == ESP_ERR_INVALID_STATE);
    run_worker();
    assert(atomic_load(&s_state) == WALLPAPER_STATE_SAVED && !s_saving);
    assert(wallpaper_available() && s_front && !memcmp(s_front, image_a, sizeof(image_a)));
    assert(slot_valid(0) && !slot_valid(1));
    assert(s_front_index == 0 && s_front_seq == 1);
    assert(lock_calls == 1);
    wallpaper_ack_result();
    assert(atomic_load(&s_state) == WALLPAPER_STATE_IDLE);
    puts("PASS first wallpaper lands in slot 0/seq 1; concurrent commit rejected");

    /* 第二次保存写另一个槽，序号递增；重启后选中最新的 B。 */
    assert(wallpaper_commit(image_b) == ESP_OK);
    run_worker();
    assert(atomic_load(&s_state) == WALLPAPER_STATE_SAVED);
    assert(slot_valid(0) && slot_valid(1));
    assert(s_front_index == 1 && s_front_seq == 2);
    assert(!memcmp(s_front, image_b, sizeof(image_b)));
    boot();
    assert(wallpaper_init() == ESP_OK);
    assert(wallpaper_available() && s_front_index == 1 && s_front_seq == 2);
    assert(!memcmp(s_front, image_b, sizeof(image_b)));
    puts("PASS second wallpaper uses the inactive slot and survives reboot");

    /* 最新槽像素损坏：启动回退到上一槽 A，不把坏图当壁纸。 */
    flash[slot_payload(1) + 100] ^= 0x5A;
    boot();
    assert(wallpaper_init() == ESP_OK);
    assert(s_front_index == 0 && s_front_seq == 1);
    assert(!memcmp(s_front, image_a, sizeof(image_a)));
    puts("PASS corrupted newest payload falls back to the older verified slot");

    /* 恢复槽 1 为有效 B，再模拟写槽 0 中途失败：旧壁纸 B 保持可用。 */
    boot();
    assert(wallpaper_init() == ESP_OK);           /* 回退读 A，front=A */
    /* 重新写一张 B 到槽 1：当前 front=A(seq1) -> 非活动槽 1。 */
    assert(wallpaper_commit(image_b) == ESP_OK);
    run_worker();
    assert(atomic_load(&s_state) == WALLPAPER_STATE_SAVED && s_front_seq == 2 &&
           !memcmp(s_front, image_b, sizeof(image_b)));
    /* 下一次写槽 0 时在第 2 次写入失败（header 已擦除、payload 半写）。 */
    writes = 0;
    fail_write = 2;
    assert(wallpaper_commit(image_c) == ESP_OK);
    run_worker();
    assert(atomic_load(&s_state) == WALLPAPER_STATE_ERROR);
    assert(!memcmp(s_front, image_b, sizeof(image_b))); /* 画面仍是旧壁纸 */
    assert(!slot_valid(0) && slot_valid(1));
    boot();
    fail_write = 0;
    assert(wallpaper_init() == ESP_OK);
    assert(s_front_index == 1 && !memcmp(s_front, image_b, sizeof(image_b)));
    puts("PASS interrupted slot write keeps the previous wallpaper and boots from it");

    /* 分区尺寸不符：初始化明确拒绝，不越界访问。 */
    boot();
    partition.size = SECTOR_BYTES;
    assert(wallpaper_init() == ESP_ERR_NOT_SUPPORTED);
    partition.size = sizeof(flash);
    puts("PASS undersized partition is rejected before any read/write");

    /* 保存成功后 front/back 交换，不产生第三块长期缓冲：启动时只有
     * front 一块，首次提交分配 back，之后来回交换。 */
    boot();
    assert(wallpaper_init() == ESP_OK);
    size_t baseline_live = live;
    assert(wallpaper_commit(image_c) == ESP_OK);
    run_worker();
    assert(live == baseline_live + WALLPAPER_PIXEL_BYTES);
    assert(atomic_load(&s_state) == WALLPAPER_STATE_SAVED);
    size_t two_buffers = live;
    assert(wallpaper_commit(image_a) == ESP_OK);
    run_worker();
    assert(live == two_buffers);
    puts("PASS front/back swap reuses the two PSRAM buffers");

    boot();
    puts("ALL WALLPAPER STORAGE TESTS PASSED");
    return 0;
}
