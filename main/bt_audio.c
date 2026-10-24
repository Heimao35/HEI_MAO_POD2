/**
 * @file bt_audio.c
 * @brief 蓝牙应用层状态机：UI 意图、扫描结果、连接状态和配对记忆。
 *
 * 分工：
 * - 本文件不包含 SDIO/寄存器代码，只维护 RAM 状态并向 bt_link 投递命令；
 * - bt_link.c 的 worker 完成 SDMMC/ESSL 收发，收到 U4 事件后调用本文件
 *   末尾的 bt_audio_link_* 发布接口；
 * - 所有发布接口可从 worker 调用，不接触 LVGL；UI 在自己的 250ms/1s
 *   定时器中轮询只读接口。
 *
 * 配对记忆：
 * - 只有 U4 确认 A2DP 连接（LINK_EVT_CONNECTED）后才记录 BDA 和名称；
 * - 记录放在 RAM，由 player_settings 的既有 3 秒合并快照写入 NVS，
 *   本文件不直接访问 Flash；
 * - 下次打开蓝牙时，若链路已就绪且存在配对记录，自动发送一次连接请求。
 *   自动连接失败只回到“已开启，未连接”，不把 UI 留在错误态。
 *
 * 线程安全：
 * - 状态/就绪/开关意图用 C11 原子量；
 * - 设备列表、连接名称、配对记录用一把短临界区互斥量保护。互斥量只在
 *   复制少量数据时持有，不调用 SDIO/LVGL/Flash，不会与 LVGL 锁形成环。
 */

#include "bt_audio.h"

#include <stdatomic.h>
#include <string.h>

#include "bt_link.h"
#include "link_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bt_audio";

/* UI 可见状态。开关意图与运行状态分开：ERROR/CONNECTING 时开关仍然打开。 */
static atomic_int s_state = BT_AUDIO_STATE_OFF;
static atomic_bool s_enabled = false;
static atomic_bool s_link_ready = false;
static atomic_bool s_scan_requested = false;
static atomic_bool s_auto_connecting = false;
/* 用户/自动连接已下发，但 bt_link 可能仍在等待 U4 停止扫描。期间
 * U4 的 UNCONNECTED/IDLE 状态事件不得把 UI 从 CONNECTING 拉回 ON。 */
static atomic_bool s_connect_waiting = false;
static atomic_bool s_initialized = false;
/* 用户音量意图（0..127 绝对音量）。蓝牙连接时由 AVRCP 应用；本机 DAC
 * 的“连接即静音”由 audio_output 单独处理，不改变这里保存的用户值。 */
static atomic_uint s_volume127 = 64;

/* 保护下面三组 RAM 数据。s_lock 由 bt_audio_init 创建。 */
static SemaphoreHandle_t s_lock;
static bt_audio_device_t s_devices[BT_AUDIO_MAX_DEVICES];
static size_t s_device_count;
static char s_connected_name[BT_AUDIO_NAME_MAX];
static uint8_t s_connected_bda[6];
static bool s_paired_valid;
static uint8_t s_paired_bda[6];
static char s_paired_name[BT_AUDIO_NAME_MAX];

/* ------------------------- 临界区辅助 ------------------------- */

static bool lock_data(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlock_data(void)
{
    xSemaphoreGive(s_lock);
}

static bool bda_is_zero(const uint8_t bda[6])
{
    return !(bda[0] | bda[1] | bda[2] | bda[3] | bda[4] | bda[5]);
}

/* 调用方必须持有 s_lock。返回设备列表中的名称或配对名称；没有则 NULL。 */
static const char *find_name_locked(const uint8_t bda[6])
{
    for (size_t i = 0; i < s_device_count; ++i) {
        if (memcmp(s_devices[i].bda, bda, 6) == 0 && s_devices[i].name[0])
            return s_devices[i].name;
    }
    if (s_paired_valid && memcmp(s_paired_bda, bda, 6) == 0 && s_paired_name[0])
        return s_paired_name;
    return NULL;
}

/* 调用方必须持有 s_lock。 */
static void set_connected_name_locked(const uint8_t bda[6])
{
    const char *name = find_name_locked(bda);
    if (name) {
        snprintf(s_connected_name, sizeof(s_connected_name), "%s", name);
    } else {
        s_connected_name[0] = '\0';
    }
}

/* ------------------------- 自动连接 ------------------------- */

/**
 * @brief 链路就绪且用户已打开时，尝试连接最近一次成功配对的设备。
 *
 * 自动连接只发起一次；失败由 bt_audio_link_error 回退到“已开启/未连接”，
 * 不会反复重连、不会覆盖用户正在发起的连接。
 */
static void try_auto_connect(void)
{
    if (!atomic_load(&s_enabled) || !atomic_load(&s_link_ready)) return;

    bt_audio_state_t state = (bt_audio_state_t)atomic_load(&s_state);
    if (state == BT_AUDIO_STATE_CONNECTED || state == BT_AUDIO_STATE_CONNECTING)
        return;

    uint8_t bda[6] = {0};
    bool valid = false;
    if (lock_data()) {
        valid = s_paired_valid;
        if (valid) memcpy(bda, s_paired_bda, sizeof(bda));
        unlock_data();
    }
    if (!valid) return;

    /* 标记自动连接，让随后的超时错误不把 UI 留在红色错误态。 */
    atomic_store(&s_auto_connecting, true);
    atomic_store(&s_scan_requested, false);
    esp_err_t err = bt_link_connect(bda);
    if (err == ESP_OK) {
        atomic_store(&s_connect_waiting, true);
        atomic_store(&s_state, BT_AUDIO_STATE_CONNECTING);
        ESP_LOGI(TAG, "Auto-connecting last paired device");
    } else {
        atomic_store(&s_auto_connecting, false);
        ESP_LOGW(TAG, "Auto-connect not started: %s", esp_err_to_name(err));
    }
}

/* ========================= 公共 UI 接口 ========================= */

esp_err_t bt_audio_init(void)
{
    if (atomic_exchange(&s_initialized, true)) return ESP_OK;

    /* 上电默认关闭，且不读取 NVS：开关状态按需求不持久化。配对记录由
     * main 在 player_settings_init 后调用 bt_audio_set_paired 恢复。 */
    atomic_store(&s_state, BT_AUDIO_STATE_OFF);
    atomic_store(&s_enabled, false);
    atomic_store(&s_link_ready, false);
    atomic_store(&s_scan_requested, false);
    atomic_store(&s_auto_connecting, false);
    atomic_store(&s_connect_waiting, false);
    atomic_store(&s_volume127, 64);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        atomic_store(&s_initialized, false);
        return ESP_ERR_NO_MEM;
    }

    s_device_count = 0;
    s_connected_name[0] = '\0';
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    s_paired_valid = false;
    memset(s_paired_bda, 0, sizeof(s_paired_bda));
    s_paired_name[0] = '\0';

    ESP_LOGI(TAG, "Bluetooth facade initialized (off by default)");
    return ESP_OK;
}

void bt_audio_set_enabled(bool enabled)
{
    if (!enabled) {
        atomic_store(&s_enabled, false);
        atomic_store(&s_state, BT_AUDIO_STATE_OFF);
        atomic_store(&s_scan_requested, false);
        atomic_store(&s_auto_connecting, false);
        atomic_store(&s_connect_waiting, false);
        /* worker 只发 DISCONNECT/MEDIA_STOP；U4 保持供电与 SDIO 链路，
         * 下次打开无需重新初始化 SDMMC。 */
        bt_link_set_enabled(false);
        if (lock_data()) {
            s_device_count = 0;
            s_connected_name[0] = '\0';
            memset(s_connected_bda, 0, sizeof(s_connected_bda));
            unlock_data();
        }
        ESP_LOGI(TAG, "Bluetooth disabled by user");
        return;
    }

    if (atomic_exchange(&s_enabled, true)) return;

    atomic_store(&s_state, BT_AUDIO_STATE_ON);
    bt_link_set_enabled(true);
    ESP_LOGI(TAG, "Bluetooth enabled by user");
    try_auto_connect();
    if (atomic_load(&s_scan_requested) && atomic_load(&s_link_ready)) {
        bt_link_scan_start();
        atomic_store(&s_state, BT_AUDIO_STATE_SCANNING);
    }
}

bool bt_audio_is_enabled(void)
{
    return atomic_load(&s_enabled);
}

bool bt_audio_link_ready(void)
{
    return atomic_load(&s_link_ready);
}

bt_audio_state_t bt_audio_get_state(void)
{
    return (bt_audio_state_t)atomic_load(&s_state);
}

void bt_audio_scan_start(void)
{
    if (!bt_audio_is_enabled()) return;

    /* A2DP Source 连接/连接中不能再发起搜索：U4 会忽略该命令，若 UI
     * 仍把状态改成 SCANNING，会与真实的“已连接”冲突。 */
    bt_audio_state_t state = (bt_audio_state_t)atomic_load(&s_state);
    if (state == BT_AUDIO_STATE_CONNECTED || state == BT_AUDIO_STATE_CONNECTING)
        return;

    atomic_store(&s_scan_requested, true);
    if (lock_data()) {
        s_device_count = 0;
        unlock_data();
    }
    bt_link_scan_start();
    /* 链路未就绪时保持 ON，UI 会显示“协处理器未就绪”，等 READY 后补发。 */
    if (atomic_load(&s_link_ready))
        atomic_store(&s_state, BT_AUDIO_STATE_SCANNING);
}

void bt_audio_scan_stop(void)
{
    atomic_store(&s_scan_requested, false);
    bt_link_scan_stop();
    if (atomic_load(&s_state) == BT_AUDIO_STATE_SCANNING)
        atomic_store(&s_state, BT_AUDIO_STATE_ON);
}

size_t bt_audio_get_devices(bt_audio_device_t *out, size_t max)
{
    if (!out || !max) return 0;
    if (!lock_data()) return 0;
    size_t count = s_device_count < max ? s_device_count : max;
    if (count) memcpy(out, s_devices, count * sizeof(out[0]));
    unlock_data();
    return count;
}

esp_err_t bt_audio_connect(const uint8_t bda[6])
{
    if (!bda) return ESP_ERR_INVALID_ARG;
    if (!bt_audio_is_enabled()) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_link_ready)) return ESP_ERR_INVALID_STATE;

    bt_audio_state_t state = (bt_audio_state_t)atomic_load(&s_state);
    /* A2DP Source 同时只支持一个 Sink；已有连接/正在连接时拒绝新请求。 */
    if (state == BT_AUDIO_STATE_CONNECTED || state == BT_AUDIO_STATE_CONNECTING)
        return ESP_ERR_INVALID_STATE;

    atomic_store(&s_scan_requested, false);
    esp_err_t err = bt_link_connect(bda);
    if (err == ESP_OK) {
        atomic_store(&s_auto_connecting, false);
        atomic_store(&s_connect_waiting, true);
        atomic_store(&s_state, BT_AUDIO_STATE_CONNECTING);
    }
    return err;
}

bool bt_audio_is_connected(void)
{
    return atomic_load(&s_state) == BT_AUDIO_STATE_CONNECTED;
}

void bt_audio_set_volume(uint8_t volume, bool muted)
{
    if (volume > 100) volume = 100;
    unsigned vol127 = muted ? 0 : ((unsigned)volume * 127U + 50U) / 100U;
    atomic_store(&s_volume127, vol127);
    /* 蓝牙响度由 bt_audio_stream 的 PCM 增益控制；这里只保存意图，
     * 避免与连接时固定到最大的 AVRCP 音量叠加成双重衰减。 */
}

size_t bt_audio_get_connected_name(char *out, size_t size)
{
    if (!out || !size) return 0;
    out[0] = '\0';
    if (!lock_data()) return 0;
    snprintf(out, size, "%s", s_connected_name);
    unlock_data();
    return strlen(out);
}

void bt_audio_set_paired(const uint8_t bda[6], const char *name)
{
    if (!bda) return;
    if (!lock_data()) return;

    if (bda_is_zero(bda)) {
        s_paired_valid = false;
        memset(s_paired_bda, 0, sizeof(s_paired_bda));
        s_paired_name[0] = '\0';
    } else {
        s_paired_valid = true;
        memcpy(s_paired_bda, bda, sizeof(s_paired_bda));
        snprintf(s_paired_name, sizeof(s_paired_name), "%s", name ? name : "");
    }
    unlock_data();
}

bool bt_audio_get_paired(uint8_t bda[6], char *name, size_t name_size)
{
    if (!bda || !name || !name_size) return false;
    memset(bda, 0, 6);
    name[0] = '\0';
    if (!lock_data()) return false;
    bool valid = s_paired_valid;
    if (valid) {
        memcpy(bda, s_paired_bda, 6);
        snprintf(name, name_size, "%s", s_paired_name);
    }
    unlock_data();
    return valid;
}

/* ===================== 链路 worker 发布接口 ===================== */

void bt_audio_link_set_ready(bool ready)
{
    bool previous = atomic_exchange(&s_link_ready, ready);
    if (previous == ready) return;

    ESP_LOGI(TAG, "U4 link %s", ready ? "ready" : "lost");

    if (!ready) {
        /* U4 复位/链路异常：不再宣称已连接，但保留配对记录供下次重连。 */
        atomic_store(&s_auto_connecting, false);
        atomic_store(&s_connect_waiting, false);
        if (bt_audio_is_enabled()) {
            if (atomic_load(&s_state) != BT_AUDIO_STATE_OFF)
                atomic_store(&s_state, BT_AUDIO_STATE_ON);
        }
        return;
    }

    if (!bt_audio_is_enabled()) return;

    /* 优先满足“打开蓝牙自动连接已配对设备”；没有配对记录时才补发
     * 进入设置页时排队的扫描请求。 */
    bool has_pair = false;
    if (lock_data()) {
        has_pair = s_paired_valid;
        unlock_data();
    }
    if (has_pair) {
        try_auto_connect();
    } else if (atomic_load(&s_scan_requested)) {
        bt_link_scan_start();
        atomic_store(&s_state, BT_AUDIO_STATE_SCANNING);
    }
}

void bt_audio_link_scan_clear(void)
{
    if (!lock_data()) return;
    s_device_count = 0;
    unlock_data();
}

void bt_audio_link_scan_add(const uint8_t bda[6], int8_t rssi, const char *name)
{
    if (!bda) return;

    char clean[BT_AUDIO_NAME_MAX];
    snprintf(clean, sizeof(clean), "%s", name ? name : "");

    if (!lock_data()) return;
    size_t index = s_device_count;
    for (size_t i = 0; i < s_device_count; ++i) {
        if (memcmp(s_devices[i].bda, bda, 6) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_device_count) {
        if (s_device_count >= BT_AUDIO_MAX_DEVICES) {
            unlock_data();
            return; /* 列表已满：保留先发现的设备，不无限增长 */
        }
        ++s_device_count;
    }
    bt_audio_device_t *dev = &s_devices[index];
    memcpy(dev->bda, bda, 6);
    dev->rssi = rssi;
    /* 结果可能没有名称（U4 只转发带名称的广播）；此时清空旧值，避免
     * 列表清空后复用槽位仍显示上一轮设备名。 */
    snprintf(dev->name, sizeof(dev->name), "%s", clean);

    /* 已连接设备可能在断开扫描后仍广播；名称变化时同步状态行。 */
    if (atomic_load(&s_state) == BT_AUDIO_STATE_CONNECTED &&
        memcmp(s_connected_bda, bda, 6) == 0 && clean[0]) {
        snprintf(s_connected_name, sizeof(s_connected_name), "%s", clean);
    }
    unlock_data();
}

void bt_audio_link_bt_state(int bt_state)
{
    if (!bt_audio_is_enabled()) return;

    switch (bt_state) {
    case LINK_BT_DISCOVERING:
        if (atomic_load(&s_scan_requested))
            atomic_store(&s_state, BT_AUDIO_STATE_SCANNING);
        break;
    case LINK_BT_CONNECTING:
        atomic_store(&s_state, BT_AUDIO_STATE_CONNECTING);
        break;
    case LINK_BT_DISCONNECTING:
        if (!atomic_load(&s_connect_waiting))
            atomic_store(&s_state, BT_AUDIO_STATE_ON);
        break;
    case LINK_BT_CONNECTED:
        /* P4 重启后 U4 可能仍保持上一次 A2DP 连接，HELLO 只会补发
         * STATE/CONNECTED 状态而不会重发 CONNECTED 事件。这里用状态
         * 寄存器恢复“已连接”，名称先取配对记录；随后若收到 CONNECTED
         * 事件再用真实 BDA/名称覆盖。 */
        if (atomic_load(&s_state) != BT_AUDIO_STATE_CONNECTED) {
            if (lock_data()) {
                if (!s_connected_name[0] && s_paired_valid) {
                    memcpy(s_connected_bda, s_paired_bda, 6);
                    snprintf(s_connected_name, sizeof(s_connected_name), "%s",
                             s_paired_name);
                }
                unlock_data();
            }
            atomic_store(&s_auto_connecting, false);
            atomic_store(&s_connect_waiting, false);
            atomic_store(&s_state, BT_AUDIO_STATE_CONNECTED);
        }
        break;
    default:
        /* P4 已请求连接但还在等 U4 停止扫描时，中间态 UNCONNECTED 不能
         * 覆盖 CONNECTING。 */
        if ((atomic_load(&s_state) == BT_AUDIO_STATE_SCANNING ||
             atomic_load(&s_state) == BT_AUDIO_STATE_CONNECTING) &&
            !atomic_load(&s_connect_waiting)) {
            atomic_store(&s_state, BT_AUDIO_STATE_ON);
        }
        break;
    }
}

void bt_audio_link_connected(const uint8_t bda[6])
{
    if (!bda) return;
    if (!bt_audio_is_enabled()) return;

    if (!lock_data()) return;
    memcpy(s_connected_bda, bda, 6);
    set_connected_name_locked(bda);
    /* 连接成功即认为配对成功：记住地址与当前已知名称，供下次自动连接。
     * 若这次连接没有可见名称，保留旧配对名称（同一 BDA 时）。 */
    s_paired_valid = true;
    /* 换了新设备且这次还不知道名称时，不能沿用上一台设备的名称。 */
    if (memcmp(s_paired_bda, bda, 6) != 0) s_paired_name[0] = '\0';
    memcpy(s_paired_bda, bda, 6);
    if (s_connected_name[0])
        snprintf(s_paired_name, sizeof(s_paired_name), "%s", s_connected_name);
    s_scan_requested = false;
    unlock_data();

    atomic_store(&s_auto_connecting, false);
    atomic_store(&s_connect_waiting, false);
    atomic_store(&s_state, BT_AUDIO_STATE_CONNECTED);
    /* 连接时不再自动发送 AVRCP 音量：AVRCP 通常比 A2DP 晚 3～4 秒
     * 连接，早发的命令可能让部分耳机断开。用户调节音量时再发送。 */
    ESP_LOGI(TAG, "A2DP connected");
}

void bt_audio_link_disconnected(void)
{
    if (lock_data()) {
        s_connected_name[0] = '\0';
        memset(s_connected_bda, 0, sizeof(s_connected_bda));
        s_scan_requested = false;
        unlock_data();
    }
    atomic_store(&s_auto_connecting, false);
    atomic_store(&s_connect_waiting, false);
    if (bt_audio_is_enabled())
        atomic_store(&s_state, BT_AUDIO_STATE_ON);
    ESP_LOGI(TAG, "A2DP disconnected");
}

void bt_audio_link_error(uint8_t code)
{
    ESP_LOGW(TAG, "U4 link error %u", (unsigned)code);
    if (!bt_audio_is_enabled()) return;

    /* U4 已连接时收到 CONNECT 会返回 busy(3)。P4 刚启动、U4 仍保持上次
     * 连接时会遇到：以 STATE/CONNECTED 为准，不把已连接误报成错误。 */
    if (code == 3 && atomic_load(&s_state) == BT_AUDIO_STATE_CONNECTED)
        return;

    atomic_store(&s_connect_waiting, false);
    if (atomic_exchange(&s_auto_connecting, false)) {
        /* 自动重连失败（设备关机/不在范围）不应把 UI 留在错误态。 */
        atomic_store(&s_state, BT_AUDIO_STATE_ON);
        if (atomic_load(&s_scan_requested) && atomic_load(&s_link_ready)) {
            bt_link_scan_start();
            atomic_store(&s_state, BT_AUDIO_STATE_SCANNING);
        }
    } else {
        atomic_store(&s_state, BT_AUDIO_STATE_ERROR);
    }
}
