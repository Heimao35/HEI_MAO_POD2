/**
 * @file wallpaper.c
 * @brief 壁纸 Flash 存储 + PSRAM 双缓冲 + 页面 canvas 挂接实现。
 *
 * 存储布局（分区 "wallpaper"，大小 0xE0000）：
 *   slot 0: [64B 头部][4KiB 保留][464*464*2 RGB565]
 *   slot 1: 同上，偏移 0x70000
 * 头部包含 magic/版本/递增序号/像素字节数/CRC/宽高/格式/头部 CRC。
 * 新壁纸只写"非活动槽"：先擦除该槽头部（使旧拷贝立即失效），再擦
 * 除/写入像素并读回 CRC，最后写头部作为提交点。任何一步失败或掉电，
 * 活动槽仍是上一张完整壁纸；下次启动选择序号最大的有效槽。
 *
 * 显示缓冲：
 * - s_front 是当前正在显示的 PSRAM 图，页面 canvas 直接引用它；
 * - s_back 是保存请求的副本，也是 Flash 写入源。
 * 保存成功后在 LVGL 锁内交换两个指针并重新设置 canvas 缓冲；旧 front
 * 变成下一次保存的 back，不做额外分配。
 *
 * 该模块不调用 SD 卡、不读 JPEG；启动加载只访问 Flash，所以无卡也能
 * 保留上一次设置的壁纸。
 */
#include "wallpaper.h"

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "wallpaper";

/* 分区名与槽位尺寸。0x70000 必须 >= 4KiB 头 + 430592 字节像素；
 * 两个槽共 0xE0000，与 partitions.csv 保持一致。 */
#define PARTITION_LABEL "wallpaper"
#define HEADER_BYTES     64U
#define SECTOR_BYTES     4096U
#define SLOT_BYTES       0x70000U
#define IO_BYTES         (16U * 1024U)
/* 运行期擦写必须切得足够小：IDF 的一次 esp_partition_erase/write 会在
 * 整段操作期间关闭 cache 并暂停另一颗核心（音频解码任务和 LVGL 都在
 * flash 中执行），单次 64KiB 擦除可停顿数百毫秒，足以把 64KiB PCM 环
 * 唱空。这里按 4KiB 扇区擦除、4KiB 小页写入，并且每 4 次操作让出
 * 一次 30ms/20ms，使连续停顿不超过约 160ms/44ms，让音频任务在间隙
 * 把 PCM 环重新填满。代价是保存总耗时增加约 1.3 秒。 */
#define WRITE_CHUNK_BYTES SECTOR_BYTES
#define ERASE_YIELD_EVERY 4U
#define ERASE_YIELD_MS    30U
#define WRITE_YIELD_EVERY 4U
#define WRITE_YIELD_MS    20U
#define HEADER_MAGIC     0x31504C57U /* little endian: "WLP1" */
#define FORMAT_VERSION   1U
#define FORMAT_RGB565    1U

/* 页面 canvas 的注册表：视频列表、图片首页、电子书首页共 3 个。
 * 多留一个槽位，避免以后新增页面时静默丢失注册。 */
#define WALLPAPER_CANVAS_MAX 4U

typedef struct {
    bool valid;
    unsigned index;
    uint64_t sequence;
    uint32_t bytes;
    uint32_t crc;
} slot_t;

static const esp_partition_t *s_part;
/* PSRAM 双缓冲。s_front 只有在 CRC 校验通过或保存成功后才算有效；
 * s_loaded 与指针分开，避免"已分配但内容无效"被当成有壁纸。 */
static uint8_t *s_front;
static uint8_t *s_back;
static bool s_loaded;
/* 头部/读回校验用的 16KiB scratch，与 lock_animation 的 Flash 校验一致。 */
static uint8_t *s_scratch;
static unsigned s_front_index;
static uint64_t s_front_seq;
static bool s_initialized;
static atomic_bool s_saving;
static atomic_int s_state;
static lv_obj_t *s_canvases[WALLPAPER_CANVAS_MAX];
static unsigned s_canvas_count;

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}

/* 头部校验和覆盖前 60 字节；最后 4 字节是头部自身的 CRC。 */
static void encode_header(uint8_t *h, const slot_t *slot)
{
    memset(h, 0, HEADER_BYTES);
    put32(h, HEADER_MAGIC);
    put32(h + 4, FORMAT_VERSION);
    put32(h + 8, (uint32_t)slot->sequence);
    put32(h + 12, (uint32_t)(slot->sequence >> 32));
    put32(h + 16, slot->bytes);
    put32(h + 20, slot->crc);
    put32(h + 24, WALLPAPER_PAD_WIDTH);
    put32(h + 28, WALLPAPER_PAD_HEIGHT);
    put32(h + 32, FORMAT_RGB565);
    put32(h + 60, esp_crc32_le(0, h, 60));
}

/**
 * @brief 读取并验证一个槽的头部（不读像素，启动时两个槽都先看头部）。
 *
 * 只接受当前格式：magic/版本/头部 CRC/尺寸/格式/像素字节数全部匹配。
 * 旧版本或损坏头部按“该槽不存在”处理，不猜测兼容格式。
 */
static bool read_header(const esp_partition_t *part, unsigned index, slot_t *slot)
{
    uint8_t h[HEADER_BYTES];
    memset(slot, 0, sizeof(*slot));
    slot->index = index;
    esp_err_t ret = esp_partition_read(part, (size_t)index * SLOT_BYTES,
                                       h, sizeof(h));
    if (ret != ESP_OK) return false;
    if (get32(h) != HEADER_MAGIC || get32(h + 4) != FORMAT_VERSION ||
        get32(h + 60) != esp_crc32_le(0, h, 60) ||
        get32(h + 24) != WALLPAPER_PAD_WIDTH ||
        get32(h + 28) != WALLPAPER_PAD_HEIGHT ||
        get32(h + 32) != FORMAT_RGB565 ||
        get32(h + 16) != WALLPAPER_PIXEL_BYTES)
        return false;
    slot->sequence = get32(h + 8) | (uint64_t)get32(h + 12) << 32;
    slot->bytes = get32(h + 16);
    slot->crc = get32(h + 20);
    if (!slot->sequence) return false;
    slot->valid = true;
    return true;
}

/** @brief 分块计算 Flash 中像素数据的 CRC，避免一次分配整块读缓冲。 */
static esp_err_t flash_crc(const esp_partition_t *part, size_t offset,
                           size_t bytes, uint32_t *crc)
{
    *crc = 0;
    while (bytes) {
        size_t n = bytes < IO_BYTES ? bytes : IO_BYTES;
        esp_err_t ret = esp_partition_read(part, offset, s_scratch, n);
        if (ret != ESP_OK) return ret;
        *crc = esp_crc32_le(*crc, s_scratch, n);
        offset += n;
        bytes -= n;
    }
    return ESP_OK;
}

/**
 * @brief 把一个槽的像素读入 s_front 并校验 CRC。
 *
 * 像素只在 CRC 通过后才被当作有效壁纸；失败时缓冲内容可能不完整，
 * 但不会注册给 canvas（s_front 仍为空/旧值由调用方保持）。
 */
static esp_err_t load_slot(const esp_partition_t *part, const slot_t *slot)
{
    if (!slot->valid) return ESP_ERR_NOT_FOUND;
    /* 先把数据读进临时指针；只有 CRC 通过才登记为 s_front。这样回退
     * 到另一槽失败时，不会留下"指针非空但内容是坏图"的状态。 */
    uint8_t *buf = s_front;
    bool allocated = false;
    if (!buf) {
        buf = heap_caps_malloc(WALLPAPER_PIXEL_BYTES,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) return ESP_ERR_NO_MEM;
        allocated = true;
    }
    esp_err_t ret = esp_partition_read(part,
                                       (size_t)slot->index * SLOT_BYTES + SECTOR_BYTES,
                                       buf, slot->bytes);
    uint32_t crc = 0;
    if (ret == ESP_OK) {
        crc = esp_crc32_le(0, buf, slot->bytes);
        if (crc != slot->crc) {
            ESP_LOGW(TAG, "Slot %u payload CRC mismatch (%08" PRIx32 "/%08" PRIx32 ")",
                     slot->index, crc, slot->crc);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (ret != ESP_OK) {
        /* 本次启动还没有任何有效壁纸：释放临时缓冲并保持 s_front 为空，
         * 让后续回退尝试重新读取（可能是另一槽）。 */
        if (allocated && !s_loaded) {
            heap_caps_free(buf);
            s_front = NULL;
        }
        return ret;
    }
    s_front = buf;
    s_loaded = true;
    s_front_index = slot->index;
    s_front_seq = slot->sequence;
    return ESP_OK;
}

esp_err_t wallpaper_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized = true;
    atomic_store(&s_state, WALLPAPER_STATE_IDLE);

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (!part) {
        /* 用户尚未烧录新分区表时不让整机启动失败：壁纸功能明确不可用，
         * 其他页面设置/播放不受影响。 */
        ESP_LOGE(TAG, "Missing wallpaper partition; flash the new partition table");
        return ESP_ERR_NOT_FOUND;
    }
    if (part->size < 2U * SLOT_BYTES || part->erase_size != SECTOR_BYTES) {
        ESP_LOGE(TAG, "Wallpaper partition size/erase size unsupported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_scratch = heap_caps_malloc(IO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scratch) return ESP_ERR_NO_MEM;
    s_part = part;

    slot_t slots[2];
    for (unsigned i = 0; i < 2; ++i) read_header(part, i, &slots[i]);
    const slot_t *newest = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        if (slots[i].valid &&
            (!newest || slots[i].sequence > newest->sequence))
            newest = &slots[i];
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (newest) ret = load_slot(part, newest);
    /* 新槽像素损坏时回退到另一槽；这是 A/B 写入的最后一道保护。 */
    if (ret != ESP_OK) {
        for (unsigned i = 0; i < 2; ++i) {
            if (&slots[i] != newest && slots[i].valid) {
                ret = load_slot(part, &slots[i]);
                if (ret == ESP_OK) break;
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No usable wallpaper stored yet (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Wallpaper loaded: slot=%u sequence=%" PRIu64,
             s_front_index, s_front_seq);
    return ESP_OK;
}

bool wallpaper_available(void)
{
    return s_loaded && s_front != NULL;
}

/**
 * @brief 把一个 canvas 指向当前壁纸缓冲。
 *
 * 没有壁纸时隐藏 canvas，页面保持原来的纯色背景；有壁纸时 464x464
 * 的 canvas 放在 460x460 的页面里，由父对象裁掉右侧/下侧 4 像素。
 * 调用方必须持有 LVGL 锁。
 */
static void apply_canvas(lv_obj_t *canvas)
{
    if (!canvas) return;
    if (s_loaded && s_front) {
        lv_canvas_set_buffer(canvas, s_front, WALLPAPER_PAD_WIDTH,
                             WALLPAPER_PAD_HEIGHT, LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(canvas);
    } else {
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_all_canvases(void)
{
    for (unsigned i = 0; i < s_canvas_count; ++i) apply_canvas(s_canvases[i]);
}

lv_obj_t *wallpaper_create_canvas(lv_obj_t *parent)
{
    if (!parent) return NULL;
    /* 必须由页面在创建其他子对象之前调用，才能位于内容下方。
     * canvas 不接触摸：没有 CLICKABLE，点击会落到页面的原控件。 */
    lv_obj_t *canvas = lv_canvas_create(parent);
    if (!canvas) return NULL;
    lv_obj_set_size(canvas, WALLPAPER_PAD_WIDTH, WALLPAPER_PAD_HEIGHT);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (s_canvas_count < WALLPAPER_CANVAS_MAX)
        s_canvases[s_canvas_count++] = canvas;
    else
        ESP_LOGW(TAG, "Too many wallpaper canvases; extra one will not update");
    apply_canvas(canvas);
    return canvas;
}

/**
 * @brief 擦写非活动槽；只在一次性 worker 中执行。
 *
 * 返回时 s_pending_index/s_pending_seq 描述已提交的新槽，供 worker 在
 * LVGL 锁内交换显示缓冲。函数不修改 s_front（旧壁纸保持可见）。
 */
static esp_err_t write_inactive_slot(unsigned *pending_index,
                                     uint64_t *pending_seq)
{
    const esp_partition_t *part = s_part;
    if (!part || !s_back) return ESP_ERR_INVALID_STATE;
    unsigned index = s_front ? (s_front_index ^ 1U) : 0U;
    uint64_t sequence = s_front_seq ? s_front_seq + 1U : 1U;
    if (sequence == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    size_t base = (size_t)index * SLOT_BYTES;

    /* 先擦头部：非活动槽若是上一次的旧壁纸，此刻起不再被视为有效；
     * 若中途掉电，启动时仍会选中活动槽的完整版本。 */
    esp_err_t ret = esp_partition_erase_range(part, base, SECTOR_BYTES);
    if (ret != ESP_OK) return ret;

    const size_t payload = WALLPAPER_PIXEL_BYTES;
    const size_t erase = (payload + SECTOR_BYTES - 1U) & ~(size_t)(SECTOR_BYTES - 1U);
    size_t offset = 0;
    unsigned sectors = 0;
    while (offset < erase) {
        ret = esp_partition_erase_range(part, base + SECTOR_BYTES + offset,
                                        SECTOR_BYTES);
        if (ret != ESP_OK) return ret;
        offset += SECTOR_BYTES;
        /* 擦除期间另一颗核心会被 IDF 的 cache 关闭流程暂停；按扇区
         * 分段并周期性让出 CPU，音频任务可在间隙补 PCM，界面也能继续
         * 刷新“正在保存”提示。 */
        if (++sectors % ERASE_YIELD_EVERY == 0)
            vTaskDelay(pdMS_TO_TICKS(ERASE_YIELD_MS));
    }

    const uint32_t crc = esp_crc32_le(0, s_back, payload);
    offset = 0;
    unsigned chunks = 0;
    while (offset < payload) {
        size_t n = payload - offset;
        if (n > WRITE_CHUNK_BYTES) n = WRITE_CHUNK_BYTES;
        ret = esp_partition_write(part, base + SECTOR_BYTES + offset,
                                  s_back + offset, n);
        if (ret != ESP_OK) return ret;
        offset += n;
        /* 写入同样分段让出：一次 esp_partition_write 内部会关闭 cache
         * 并暂停另一颗核心，连续写会把音频/界面挡在 flash 操作里。 */
        if (++chunks % WRITE_YIELD_EVERY == 0)
            vTaskDelay(pdMS_TO_TICKS(WRITE_YIELD_MS));
    }

    /* 读回整块像素并核对 CRC：只有 Flash 中确实与请求一致才提交头部。 */
    uint32_t written_crc = 0;
    ret = flash_crc(part, base + SECTOR_BYTES, payload, &written_crc);
    if (ret != ESP_OK) return ret;
    if (written_crc != crc) {
        ESP_LOGE(TAG, "Wallpaper readback CRC mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }

    slot_t next = {.index = index, .sequence = sequence, .bytes = (uint32_t)payload,
                   .crc = crc, .valid = true};
    uint8_t header[HEADER_BYTES], readback[HEADER_BYTES];
    encode_header(header, &next);
    ret = esp_partition_write(part, base, header, sizeof(header));
    if (ret != ESP_OK) return ret;
    ret = esp_partition_read(part, base, readback, sizeof(readback));
    if (ret != ESP_OK) return ret;
    if (memcmp(header, readback, sizeof(header))) return ESP_ERR_INVALID_RESPONSE;
    *pending_index = index;
    *pending_seq = sequence;
    return ESP_OK;
}

/**
 * @brief 保存 worker：Flash 擦写 -> LVGL 锁内交换 front/back -> 发布状态。
 *
 * 交换在 LVGL 锁内完成：所有 canvas 先指向新缓冲，旧 front 才成为下一
 * 次的 back，因此绘制线程不会看到"指针已换、canvas 未换"的中间状态。
 * 锁最多重试约 5 秒；仍拿不到时壁纸已经写入 Flash，本次不更新画面，
 * 下次启动会正常加载。
 */
static void wallpaper_worker(void *arg)
{
    (void)arg;
    unsigned index = 0;
    uint64_t sequence = 0;
    esp_err_t ret = write_inactive_slot(&index, &sequence);
    if (ret == ESP_OK) {
        bool applied = false;
        for (unsigned attempt = 0; attempt < 10 && !applied; ++attempt) {
            if (lvgl_port_lock(500)) {
                uint8_t *old_front = s_front;
                s_front = s_back;
                s_back = old_front;
                s_loaded = true;
                s_front_index = index;
                s_front_seq = sequence;
                apply_all_canvases();
                lvgl_port_unlock();
                applied = true;
            }
        }
        if (!applied)
            ESP_LOGW(TAG, "Wallpaper saved but UI update deferred to reboot");
        ESP_LOGI(TAG, "Wallpaper saved: slot=%u sequence=%" PRIu64,
                 index, sequence);
    } else {
        ESP_LOGE(TAG, "Wallpaper save failed: %s", esp_err_to_name(ret));
    }
    atomic_store(&s_state, ret == ESP_OK ? WALLPAPER_STATE_SAVED
                                        : WALLPAPER_STATE_ERROR);
    atomic_store(&s_saving, false);
    vTaskDelete(NULL);
}

esp_err_t wallpaper_commit(const uint8_t *pixels)
{
    if (!pixels || !s_part) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    /* 原子抢占：重复点击确认/后台仍在擦写时直接拒绝，不排队第二份。 */
    if (!atomic_compare_exchange_strong(&s_saving, &expected, true))
        return ESP_ERR_INVALID_STATE;
    if (!s_back) {
        s_back = heap_caps_malloc(WALLPAPER_PIXEL_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_back) {
            atomic_store(&s_saving, false);
            return ESP_ERR_NO_MEM;
        }
    }
    /* 在 LVGL 锁内复制像素：调用方提供的指针来自图片后台缓冲，不能假定
     * 它在本次 UI 回调返回后仍然有效。430KB 的 PSRAM 拷贝约数毫秒。 */
    memcpy(s_back, pixels, WALLPAPER_PIXEL_BYTES);
    atomic_store(&s_state, WALLPAPER_STATE_SAVING);
    if (xTaskCreate(wallpaper_worker, "wallpaper", 6144, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_saving, false);
        atomic_store(&s_state, WALLPAPER_STATE_ERROR);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

wallpaper_state_t wallpaper_status(void)
{
    return (wallpaper_state_t)atomic_load(&s_state);
}

void wallpaper_ack_result(void)
{
    int expected = WALLPAPER_STATE_SAVED;
    atomic_compare_exchange_strong(&s_state, &expected, WALLPAPER_STATE_IDLE);
    expected = WALLPAPER_STATE_ERROR;
    atomic_compare_exchange_strong(&s_state, &expected, WALLPAPER_STATE_IDLE);
}
