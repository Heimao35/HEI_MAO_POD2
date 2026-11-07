/**
 * @file usb_audio.c
 * @brief USB 声卡模式：UAC2 设备 + 现有 I2S/DAC PCM 会话
 *
 * 数据通路：
 *   PC ──USB HS 等时OUT──> TinyUSB/UAC FIFO ──output_cb──>
 *   audio_output 的 PCM 会话（与视频共用 64KiB 内部 ring / I2S / CS43131）
 *
 * 设计要点：
 * 1. USB 只提供 PCM，不做压缩解码。UAC 的 output_cb 只做“拷贝进 ring”，
 *    绝不阻塞、不访问 LVGL/SD/I2C。组件用一个专用 SPK 任务和单缓冲接收
 *    每 1ms 的数据，回调阻塞会导致下一包覆盖缓冲而丢帧。
 * 2. 组件的 ISO feedback 依据自己的 FIFO 水位调节主机发送速率，而这个
 *    FIFO 是按 USB SOF（主机时钟）排空的，看不到 CS43131 的 DAC 时钟。
 *    主机与板上 24.576MHz 晶振存在几十 ppm 偏差，长时间播放会让 ring
 *    缓慢漂移。回调每 100ms 检查一次 ring 水位，超出滞回带时丢/补 1 帧
 *    （96kHz 下 1 帧 = 10.4us，听感不可闻；修正能力约 ±104 ppm）。
 * 3. 只有 VBUS 存在才初始化/连接 USB；只有 tud_ready()（主机完成枚举）
 *    才认为“握手成功”，之后才暂停本地音乐、降充电电流、打开 PCM 会话。
 *    充电器（D+/D- 未接或短接）不会完成枚举，因此不会被误判为主机。
 * 4. 退出模式或拔线：立即停止 PCM 会话、恢复 800mA、软断开 D+ 上拉。
 */

#include "usb_audio.h"

#include "audio_output.h"
#include "music_player_adapter.h"
#include "pmic_axp2101.h"
#include "system_power.h"
#include "tusb.h"
#include "usb_device_uac.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stddef.h>

static const char *TAG = "usb_audio";

/* 固定规格：与 sdkconfig 的 CONFIG_UAC_SAMPLE_RATE/BYTES_PER_SAMPLE 对应。
 * 修改这里必须同步修改 sdkconfig（描述符在编译期生成）。 */
#define USB_AUDIO_RATE          96000U
#define USB_AUDIO_BITS          24U
#define USB_AUDIO_CHANNELS      2U
#define USB_AUDIO_FRAME_BYTES   (USB_AUDIO_CHANNELS * (USB_AUDIO_BITS / 8U))
/* 启动水位：96kHz/24bit 立体声约 576B/ms，16KiB≈28ms。比视频的
 * 63KiB 高水位低，降低声卡延迟。漂移修正也以该水位为目标。 */
#define USB_AUDIO_START_BYTES   (16U * 1024U)
/* 漂移修正：每100ms最多修正1帧，滞回带±2KiB（约3.5ms）。 */
#define USB_AUDIO_SLIP_INTERVAL_MS 100U
#define USB_AUDIO_SLIP_HYST_BYTES  (2U * 1024U)
/* 写入ring时最多等待的毫秒数：ring满说明主机送数快于DAC，这里阻塞
 * SPK任务即给UAC FIFO施加背压，让组件的FIFO_COUNT反馈下调主机速率。 */
#define USB_AUDIO_WRITE_TIMEOUT_MS 10U

static TaskHandle_t s_task;
/* 用户停留在 USB 声卡模式（由 UI 设置）。 */
static atomic_bool s_mode;
/* PCM 会话已打开（UAC 回调据此决定是否接收数据）。 */
static atomic_bool s_session;
/* 主机枚举完成（tud_ready）；仅用于日志/界面。 */
static atomic_bool s_host_ready;
/* 统计：最近窗口内收到的总帧数、因 ring 满丢弃的帧数、主动漂移修正帧数。 */
static atomic_uint s_frames_in;
static atomic_uint s_frames_dropped;
static atomic_uint s_frames_slipped;
/* 上次漂移检查 / 上次收到音频的时间（仅 USB SPK 任务访问）。 */
static TickType_t s_slip_at;
static TickType_t s_last_cb_at;
/* 主机停止/重新开始播放的判定阈值：超过该时间没有回调即视为新一轮流。 */
#define USB_AUDIO_STREAM_GAP_MS 300U

/* worker 私有状态，只在 usb_audio_task 中读写。组件 uac_device_init 在失败
 * 后仍保留内部句柄，重试会返回“已初始化”但栈并未工作，因此失败只记录
 * 一次并停止尝试，避免误判为成功。 */
static bool s_uac_inited;
static bool s_uac_failed;
/* 音频会话打开失败后的节流时间：open 内部最多等待1秒，不能每100ms重试。 */
static TickType_t s_session_retry_at;

/* UAC 扬声器回调：运行在 TinyUSB 任务上下文。只做无阻塞拷贝，绝不访问
 * LVGL/SD/I2C，也不等待任务通知以外的锁。 */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (!atomic_load_explicit(&s_session, memory_order_acquire)) return ESP_OK;
    const size_t frames = len / USB_AUDIO_FRAME_BYTES;
    if (!frames) return ESP_OK;

    uint8_t *data = buf;
    size_t count = frames;
    const TickType_t now = xTaskGetTickCount();

    /* 主机暂停或关闭流后重新开始：先停止TX，让 ring 重新积累到启动水位
     * 再恢复，避免新一轮播放开头的连续欠载/爆音。播放中的正常抖动远小于
     * 该间隔，不会触发。 */
    if ((TickType_t)(now - s_last_cb_at) > pdMS_TO_TICKS(USB_AUDIO_STREAM_GAP_MS))
        audio_output_video_play(false);
    s_last_cb_at = now;

    /* 每100ms最多做一次 ±1 帧的漂移修正。丢帧/补帧都只影响10.4us，
     * 远小于音频帧本身的可闻失真；修正能力和频度都远低于产生爆音。
     * 预缓冲阶段（play=false）不修正，否则会把刚积累的数据丢掉。 */
    if (audio_output_video_play_requested() &&
        (TickType_t)(now - s_slip_at) >= pdMS_TO_TICKS(USB_AUDIO_SLIP_INTERVAL_MS)) {
        s_slip_at = now;
        const size_t queued = audio_output_pcm_queued();
        if (queued > USB_AUDIO_START_BYTES + USB_AUDIO_SLIP_HYST_BYTES && count > 1) {
            /* ring 偏高：主机时钟略快，丢弃本批第一帧。 */
            data += USB_AUDIO_FRAME_BYTES;
            --count;
            atomic_fetch_add(&s_frames_slipped, 1);
        } else if (queued + USB_AUDIO_SLIP_HYST_BYTES < USB_AUDIO_START_BYTES) {
            /* ring 偏低：DAC 时钟略快，重复第一帧补足。 */
            audio_output_video_write(data, 1, 0);
            atomic_fetch_add(&s_frames_slipped, 1);
        }
    }

    /* ring满时最多等待10ms：这是反馈闭环的背压，不能无限等待，也不能
     * 像之前那样立刻丢弃，否则主机会被FIFO反馈持续加速（实测+0.8%）。 */
    const size_t done = audio_output_video_write(data, count, USB_AUDIO_WRITE_TIMEOUT_MS);
    atomic_fetch_add(&s_frames_in, (unsigned)frames);
    if (done < count) atomic_fetch_add(&s_frames_dropped, (unsigned)(count - done));
    /* 新一轮流：ring 已重新积累到启动水位，恢复I2S输出。 */
    if (!audio_output_video_play_requested() &&
        audio_output_pcm_queued() >= USB_AUDIO_START_BYTES)
        audio_output_video_play(true);
    return ESP_OK;
}

/* 本地音频是否已被声卡流程暂停（worker-only）。 */
static bool s_local_audio_held;

/*
 * 暂停本地音乐并等待解码器/文件真正关闭。
 *
 * 必须在初始化UAC之前完成：播放过音乐时解码器仍占用内部内存，UAC的
 * 结构体+两个4KiB任务栈会因此分配失败（实测 usb_spk task 创建失败，
 * 随后主机挂载触发空句柄通知断言重启）。暂停顺序与视频播放相同：先停
 * 播放意图并等媒体worker空闲，再释放audio_output的静默屏障，最后等
 * audio_output报告不再活跃（解码器已释放）。
 */
static void local_audio_off(void)
{
    if (s_local_audio_held) return;
    audio_output_set_playing(false);
    bool paused = false;
    for (unsigned i = 0; i < 150; ++i) { /* 最多约3秒 */
        if (music_player_adapter_pause(true)) { paused = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!paused)
        ESP_LOGW(TAG, "Media worker still busy; USB audio may contend for SD/CPU");
    audio_output_quiesce(false);
    for (unsigned i = 0; i < 100 && audio_output_is_active(); ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_local_audio_held = true;
    ESP_LOGI(TAG, "Local audio stopped: internal free %u B, DMA free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static void local_audio_restore(void)
{
    if (!s_local_audio_held) return;
    s_local_audio_held = false;
    /* 关机/低电流程自己持有媒体暂停位，这里不能解除。 */
    if (!system_power_is_active()) music_player_adapter_pause(false);
}

/* PCM 会话是否已打开（worker-only）。打开会话会在音频worker中真正关闭
 * 音乐解码器/文件并创建16KiB I2S DMA；只“暂停”音乐时解码器仍驻留，
 * 内部RAM只剩几KB，UAC的任务栈无法创建（实测失败并曾引发空句柄断言）。 */
static bool s_pcm_open;
static TickType_t s_pcm_retry_at;
static bool s_pcm_retry_logged;

static bool open_pcm(void)
{
    if (s_pcm_open) return true;
    local_audio_off();
    const audio_output_pcm_cfg_t cfg = {
        .sample_rate = USB_AUDIO_RATE,
        .channels = USB_AUDIO_CHANNELS,
        .bits = USB_AUDIO_BITS,
        .start_bytes = USB_AUDIO_START_BYTES,
        /* 声卡模式独占本机DAC：即使蓝牙已连接，也不把PCM转发给蓝牙。 */
        .no_bluetooth = true,
    };
    esp_err_t ret = audio_output_video_open(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB audio PCM open failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_pcm_open = true;
    ESP_LOGI(TAG, "USB audio PCM prepared: %uHz/%ubit/%uch, internal free %u B",
             (unsigned)USB_AUDIO_RATE, (unsigned)USB_AUDIO_BITS, (unsigned)USB_AUDIO_CHANNELS,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void close_pcm(void)
{
    if (!s_pcm_open) return;
    s_pcm_open = false;
    audio_output_video_close();
}

/* 主机握手成功：会话数据通路在UAC初始化前已就绪，这里只开始计费/降流。 */
static bool start_session(void)
{
    if (!open_pcm()) return false;
    /* 本地补丁：握手前可能有残留数据（上一次会话的 ring 内容或主机提前
     * 送来的帧）。此时 s_session 仍为 false，回调不会写入，可以安全地
     * 停止 TX 并清空 PCM ring，保证新会话从空缓冲开始。 */
    audio_output_video_flush();
    atomic_store(&s_frames_in, 0);
    atomic_store(&s_frames_dropped, 0);
    atomic_store(&s_frames_slipped, 0);
    s_slip_at = xTaskGetTickCount();
    s_last_cb_at = s_slip_at;
    atomic_store(&s_session, true);
    /* 握手成功且音频链路就绪后，才把充电电流降到 300mA，避免主机
     * USB2 端口（通常500mA）被 800mA 充电+系统负载拉垮。 */
    pmic_axp2101_set_charge_current_ma(300);
    ESP_LOGI(TAG, "USB audio session started: %uHz/%ubit/%uch, charge 300mA",
             (unsigned)USB_AUDIO_RATE, (unsigned)USB_AUDIO_BITS, (unsigned)USB_AUDIO_CHANNELS);
    return true;
}

static void stop_session(void)
{
    const bool was_session = atomic_exchange(&s_session, false);
    close_pcm();
    if (was_session) {
        pmic_axp2101_set_charge_current_ma(800);
        ESP_LOGI(TAG, "USB audio session stopped; charge restored to 800mA");
    }
    local_audio_restore();
}

static void usb_audio_task(void *arg)
{
    (void)arg;
    TickType_t diag_at = xTaskGetTickCount();
    for (;;) {
        const bool mode = atomic_load(&s_mode);
        const bool vbus = pmic_axp2101_vbus_present();

        if (!mode) {
            /* 退出模式：停止音频（会恢复充电电流），并反初始化UAC释放内部RAM。
             * I2S通道不释放，下一次进入声卡模式直接复用，避免重复16KiB分配。 */
            stop_session();
            if (s_uac_inited) {
                uac_device_deinit();
                s_uac_inited = false;
            }
            local_audio_restore();
            atomic_store(&s_host_ready, false);
        } else if (vbus) {
            /* 只有检测到有效外部5V才尝试USB；充电器不会完成枚举。
             * 先打开PCM会话（真正关闭音乐解码器并创建I2S DMA），再初始化
             * UAC；失败每5秒重试，避免每100ms重复尝试。 */
            if (!s_uac_inited && !s_uac_failed && !s_pcm_open) {
                const TickType_t now = xTaskGetTickCount();
                if (!s_pcm_retry_at ||
                    (TickType_t)(now - s_pcm_retry_at) >= pdMS_TO_TICKS(5000)) {
                    if (open_pcm()) {
                        s_pcm_retry_at = 0;
                        s_pcm_retry_logged = false;
                    } else {
                        s_pcm_retry_at = now;
                        if (!s_pcm_retry_logged)
                            ESP_LOGW(TAG, "USB audio PCM not ready; retrying every 5s");
                        s_pcm_retry_logged = true;
                    }
                }
            }
            if (!s_uac_inited && !s_uac_failed && s_pcm_open) {
                /* 记录初始化前后的内部/DMA余量，便于诊断内存问题。 */
                ESP_LOGI(TAG, "UAC init: internal free %u B, DMA free %u B before init",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
                uac_device_config_t cfg = {0};
                cfg.output_cb = uac_output_cb;
                cfg.input_cb = NULL;
                cfg.set_mute_cb = NULL;
                cfg.set_volume_cb = NULL;
                cfg.cb_ctx = NULL;
                esp_err_t ret = uac_device_init(&cfg);
                if (ret == ESP_OK) {
                    s_uac_inited = true;
                    ESP_LOGI(TAG, "UAC device initialized (USB HS device mode), DMA free %u B",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
                } else {
                    /* 初始化失败后保持失败状态，重启后再试，避免重复创建任务。 */
                    s_uac_failed = true;
                    ESP_LOGE(TAG, "UAC init failed: %s; USB audio unavailable until reboot",
                             esp_err_to_name(ret));
                }
            }
            if (s_uac_inited) {
                if (!tud_connected()) tud_connect();
                const bool ready = tud_ready();
                atomic_store(&s_host_ready, ready);
                if (ready && !atomic_load(&s_session)) {
                    const TickType_t now = xTaskGetTickCount();
                    if (!s_session_retry_at ||
                        (TickType_t)(now - s_session_retry_at) >= pdMS_TO_TICKS(5000)) {
                        s_session_retry_at = start_session() ? 0 : now;
                    }
                } else if (!ready && atomic_load(&s_session)) {
                    /* 拔线后VBUS可能因PMIC采样滞后仍为true；以USB状态为准
                     * 立即停会话，避免主机消失后仍占着I2S/DAC和300mA。 */
                    stop_session();
                }
            }
        } else {
            /* 有线拔除：停止会话、恢复充电电流，并反初始化UAC释放内部RAM。
             * I2S通道保留；重新插线后会重新初始化UAC并复用该通道。 */
            atomic_store(&s_host_ready, false);
            stop_session();
            if (s_uac_inited) {
                uac_device_deinit();
                s_uac_inited = false;
            }
            local_audio_restore();
        }

        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - diag_at) >= pdMS_TO_TICKS(5000)) {
            diag_at = now;
            if (atomic_load(&s_session)) {
                const unsigned in = atomic_exchange(&s_frames_in, 0);
                const unsigned dropped = atomic_exchange(&s_frames_dropped, 0);
                const unsigned slipped = atomic_exchange(&s_frames_slipped, 0);
                ESP_LOGI(TAG, "USB PCM: %u frames/5s, dropped=%u, drift_slip=%u",
                         in, dropped, slipped);
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

esp_err_t usb_audio_init(void)
{
    if (s_task) return ESP_OK;
    if (xTaskCreate(usb_audio_task, "usb_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void usb_audio_mode_enter(void)
{
    atomic_store(&s_mode, true);
    if (s_task) xTaskNotifyGive(s_task);
}

void usb_audio_mode_leave(void)
{
    /* 先落模式位，worker 会停止会话并恢复充电；UI 不等待 USB 拆除完成。 */
    atomic_store(&s_mode, false);
    if (s_task) xTaskNotifyGive(s_task);
}

bool usb_audio_host_ready(void)
{
    return atomic_load(&s_host_ready);
}

uint8_t usb_audio_cable_status(void)
{
    if (atomic_load(&s_host_ready)) return 2;      /* 主机已枚举 */
    if (pmic_axp2101_vbus_present()) return 1;     /* 有VBUS，但还不是主机 */
    return 0;                                      /* 没有检测到USB插入 */
}
