/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "tusb.h"
#include "uac_config.h"
#include "usb_device_uac.h"
#include "uac_descriptors.h"

static const char *TAG = "usbd_uac";


/* TinyUSB 根端口：P4 高速口是 RHPORT1，其余目标为 RHPORT0。 */
#if defined(CONFIG_TINYUSB_RHPORT_HS) && defined(CONFIG_IDF_TARGET_ESP32P4)
#define UAC_TINYUSB_RHPORT 1
#else
#define UAC_TINYUSB_RHPORT 0
#endif

/* 本地补丁：每次主机开流（alt=1）后丢弃前 N 毫秒的数据。Windows 在设备
 * 重新枚举/重新开流时会把上次会话残留的缓冲补发过来，表现为“一连接声卡
 * 就自动播放一段旧音频”。实测 300ms 窗口内主机就送来了 295ms 数据且之后
 * 仍有残留，说明补发时长超过 300ms，这里提高到 1000ms。
 * 窗口只在开流时生效，播放中途不受影响；代价是开流后 1 秒内的真实音频
 * 也会被丢弃（正常使用中流会保持打开，不会每次播放都触发）。 */
#define UAC_STREAM_DISCARD_MS 1000
/* 24bit/2ch 的帧大小，用于统计丢弃了多少音频帧。 */
#define SPK_FRAME_BYTES ((size_t)SPEAK_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX)

const uint32_t sample_rates[] = {DEFAULT_SAMPLE_RATE};

#define N_SAMPLE_RATES  TU_ARRAY_SIZE(sample_rates)

enum {
    VOLUME_CTRL_0_DB = 0,
    VOLUME_CTRL_10_DB = 2560,
    VOLUME_CTRL_20_DB = 5120,
    VOLUME_CTRL_30_DB = 7680,
    VOLUME_CTRL_40_DB = 10240,
    VOLUME_CTRL_50_DB = 12800,
    VOLUME_CTRL_60_DB = 15360,
    VOLUME_CTRL_70_DB = 17920,
    VOLUME_CTRL_80_DB = 20480,
    VOLUME_CTRL_90_DB = 23040,
    VOLUME_CTRL_100_DB = 25600,
    VOLUME_CTRL_SILENCE = 0x8000,
};

// Resolution per format
const uint8_t spk_resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX};
const uint8_t mic_resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX};

typedef struct {
    usb_phy_handle_t phy_hdl;
    uac_device_config_t user_cfg;
    int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];         // SPK feature unit (RX = OUT = SPK), +1 for master channel 0
    int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];      // SPK feature unit (RX = OUT = SPK), +1 for master channel 0
    uint8_t mic_buf1[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ];      // Buffer for microphone data
    uint8_t mic_buf2[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ];      // Buffer for microphone data
    /* 本地补丁：数据不再在中断/类回调里立刻搬走，而是留在TinyUSB的
     * 输出FIFO中，由SPK任务按1ms块取出。这样FIFO水位反映“主机送数 vs
     * DAC消费”，组件的FIFO_COUNT反馈才能把主机速率锁到声卡时钟；上游
     * 立即抽空FIFO会让反馈一直要求主机加速，实测快0.8%并持续丢帧。
     * spk_buf 只作为一次读取1ms数据的暂存。 */
    uint8_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ];
    uint8_t func_id;                                             // UAC功能索引（回调给出）
    uint8_t *mic_buf_write;                                      // Pointer to the buffer to write to
    uint8_t *mic_buf_read;                                       // Pointer to the buffer to read from
    int mic_data_size;
    int spk_itf_num;
    int mic_itf_num;
    uint8_t spk_resolution;
    uint8_t mic_resolution;
    uint32_t current_sample_rate;                                // Current resolution, update on format change
    TaskHandle_t mic_task_handle;
    TaskHandle_t spk_task_handle;
    /* 本地补丁：保存TinyUSB任务句柄，deinit时需要删除它，否则任务会继续
     * 在已拆除的TinyUSB栈上调用tud_task()。 */
    TaskHandle_t tinyusb_task_handle;
    /* SPK任务在安全点退出时置位，deinit轮询它（volatile，跨任务可见）。 */
    volatile bool spk_exited;
    size_t spk_bytes_per_ms;
    size_t mic_bytes_per_ms;
    volatile bool spk_active;
    volatile bool mic_active;
    /* 本地补丁：主机开流（alt=1）后的一段时间内丢弃收到的数据。Windows
     * 在设备重新枚举/重新开流时可能补发上次会话残留的缓冲，表现为声卡
     * 一连接就“自动播放一段旧音频”。窗口内数据不送声卡。 */
    int64_t spk_discard_until_us;
    bool spk_discard_logged;
    /* 本次开流窗口内丢弃的帧数，窗口结束后打印一次，便于判断主机补发了
     * 多少残留音频。 */
    uint32_t spk_discard_frames;
    /* 本地补丁：反初始化标志。SPK任务在安全点自行退出，避免在调用
     * output_cb（持有音频临界区）时被vTaskDelete强行删除。 */
    volatile bool shutdown;
} uac_device_t;

static uac_device_t *s_uac_device = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define UAC_ENTER_CRITICAL()    portENTER_CRITICAL(&s_mux)
#define UAC_EXIT_CRITICAL()     portEXIT_CRITICAL(&s_mux)

static void usb_phy_init(void)
{
    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
#if CONFIG_TINYUSB_RHPORT_HS
        .otg_speed = USB_PHY_SPEED_HIGH,
#endif
    };
    usb_new_phy(&phy_conf, &s_uac_device->phy_hdl);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        tud_task();
    }
}

#if !CONFIG_USB_DEVICE_UAC_AS_PART
// Invoked when device is mounted
void tud_mount_cb(void)
{
    s_uac_device->spk_active = false;
    s_uac_device->mic_active = false;
    ESP_LOGI(TAG, "USB mounted");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB unmounted");
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_uac_device->spk_active = false;
    s_uac_device->mic_active = false;
    ESP_LOGI(TAG, "USB suspended");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
}
#endif

// Helper for clock get requests
static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            TU_LOG1("Clock get current freq %lu\r\n", s_uac_device->current_sample_rate);
            audio_control_cur_4_t curf = { (int32_t) tu_htole32(s_uac_device->current_sample_rate) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        } else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
                .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
            };
            TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                rangef.subrange[i].bMin = (int32_t) sample_rates[i];
                rangef.subrange[i].bMax = (int32_t) sample_rates[i];
                rangef.subrange[i].bRes = 0;
                TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int)rangef.subrange[i].bMin, (int)rangef.subrange[i].bMax, (int)rangef.subrange[i].bRes);
            }

            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = {
            .bCur = 1
        };
        TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }

    TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n", request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
}

// Helper for clock set requests
static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;

    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));

        uint32_t target_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
        TU_LOG1("Clock set current freq: %ld\r\n", target_sample_rate);

        if (target_sample_rate != s_uac_device->current_sample_rate) {
            // For now, we only support one sample rate
            return false;
        }

        return true;
    } else {
        TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
                request->bEntityID, request->bControlSelector, request->bRequest);
        return false;
    }
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    // Set feedback method to fifo counting
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = s_uac_device->current_sample_rate;

    ESP_LOGD(TAG, "Feedback method: %d, sample freq: %"PRIu32"", feedback_param->method, feedback_param->sample_freq);
}

// Helper for feature unit get requests
static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
{
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = {
            .bCur = s_uac_device->mute[request->bChannelNumber]
        };
        TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (UAC2_ENTITY_SPK_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
            };
            TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
                    range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = {
                .bCur = tu_htole16(s_uac_device->volume[request->bChannelNumber])
            };
            TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);

    return false;
}

static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
{
    (void)rhport;

    TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        s_uac_device->mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        TU_LOG1("Set speaker channel %d Mute: %d\r\n", request->bChannelNumber, s_uac_device->mute[request->bChannelNumber]);
        if (s_uac_device->user_cfg.set_mute_cb) {
            s_uac_device->user_cfg.set_mute_cb(s_uac_device->mute[request->bChannelNumber], s_uac_device->user_cfg.cb_ctx);
        }

        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        s_uac_device->volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
        int volume_db = s_uac_device->volume[request->bChannelNumber] / 256; // Convert to dB
        int volume = (volume_db + 50) * 2; // Map to range 0 to 100
        TU_LOG1("Set speaker channel %d volume: %d dB (%d)\r\n", request->bChannelNumber, volume_db, volume);
        if (s_uac_device->user_cfg.set_volume_cb) {
            s_uac_device->user_cfg.set_volume_cb(volume, s_uac_device->user_cfg.cb_ctx);
        }
        return true;
    } else {
        TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
                request->bEntityID, request->bControlSelector, request->bRequest);
        return false;
    }
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_get_request(rhport, request);
    }
    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
        return tud_audio_feature_unit_get_request(rhport, request);
    } else {
        TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
                request->bEntityID, request->bControlSelector, request->bRequest);
    }
    return false;
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
        return tud_audio_feature_unit_set_request(rhport, request, buf);
    }
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return tud_audio_clock_set_request(rhport, request, buf);
    }
    TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);

    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;

    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

#if SPEAK_CHANNEL_NUM
    if (s_uac_device->spk_itf_num == itf && alt == 0) {
        TU_LOG2("Speaker interface closed");
        s_uac_device->spk_active = false;
        /* 关闭时丢弃上游FIFO里未消费的数据，下一次 alt=1 从头开始。 */
        tud_audio_n_clear_ep_out_ff(0);
    }
#endif

#if MIC_CHANNEL_NUM
    if (s_uac_device->mic_itf_num == itf && alt == 0) {
        TU_LOG2("Microphone interface closed");
        s_uac_device->mic_data_size = 0;
        s_uac_device->mic_active = false;
    }
#endif

    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    TU_LOG2("Set interface %d alt %d\r\n", itf, alt);

#if SPEAK_CHANNEL_NUM
    if (s_uac_device->spk_itf_num == itf && alt != 0) {
        /* 新一轮数据流：清空上游FIFO，避免上一次未消费的数据混入。 */
        tud_audio_n_clear_ep_out_ff(0);
        /* 本地补丁：每次开流都屏蔽一小段数据。主机（Windows）既可能在
         * 设备重新枚举后补发残留，也可能在重新打开流时才补发，因此不能
         * 只对“第一次开流”生效。窗口很短，播放中途不受影响。 */
        s_uac_device->spk_discard_until_us =
            esp_timer_get_time() + (int64_t)UAC_STREAM_DISCARD_MS * 1000;
        s_uac_device->spk_discard_logged = false;
        s_uac_device->spk_discard_frames = 0;
        s_uac_device->spk_resolution = spk_resolutions_per_format[alt - 1];
        s_uac_device->spk_active = true;
        s_uac_device->spk_bytes_per_ms = s_uac_device->current_sample_rate / 1000 * SPEAK_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX;
        /* 本地补丁：SPK任务创建失败时句柄为空，通知会触发FreeRTOS断言。 */
        if (s_uac_device->spk_task_handle) xTaskNotifyGive(s_uac_device->spk_task_handle);
        TU_LOG1("Speaker interface %d-%d opened", itf, alt);
        printf("Speaker interface %d-%d opened\n", itf, alt);
    }
#endif

#if MIC_CHANNEL_NUM
    if (s_uac_device->mic_itf_num == itf && alt != 0) {
        s_uac_device->mic_data_size = 0;
        s_uac_device->mic_resolution = mic_resolutions_per_format[alt - 1];
        s_uac_device->mic_active = true;
        s_uac_device->mic_bytes_per_ms = s_uac_device->current_sample_rate / 1000 * MIC_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX;
        // Prefill to the flow-control setpoint (depth/2): the controller only
        // corrects ~0.4% per packet, so from an empty FIFO it climbs for
        // seconds, sending audible zero-length packets on the way. Mirrors
        // the speaker path's buffered start.
        tu_fifo_t *in_ff = tud_audio_get_ep_in_ff();
        if (in_ff != NULL) {
            static const uint8_t zeros[64] = {0};
            uint16_t want = tu_fifo_depth(in_ff) / 2;
            while (want > 0) {
                uint16_t chunk = (want > sizeof(zeros)) ? (uint16_t)sizeof(zeros) : want;
                if (tud_audio_write(zeros, chunk) != chunk) {
                    break;
                }
                want -= chunk;
            }
        }
        xTaskNotifyGive(s_uac_device->mic_task_handle);
        TU_LOG1("Microphone interface %d-%d opened", itf, alt);
        printf("Microphone interface %d-%d opened\n", itf, alt);
    }
#endif

    return true;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)ep_out;
    (void)cur_alt_setting;

    /* 本地补丁：没有SPK任务时直接丢弃，避免空句柄通知导致断言重启。 */
    if (s_uac_device == NULL || s_uac_device->spk_task_handle == NULL) return true;

    static int64_t last_time = 0;
    int64_t now = esp_timer_get_time();

    /* 新一轮播放（间隔超过阈值）：丢弃上一次残留的FIFO数据。 */
    if (now - last_time > 100 * CONFIG_UAC_SPK_NEW_PLAY_INTERVAL) {
        tud_audio_n_clear_ep_out_ff(func_id);
    }
    last_time = now;

    /* 本地补丁：数据保留在TinyUSB输出FIFO里，由SPK任务按1ms块读取。
     * 这里只在攒够约1ms数据时通知一次：既避免125us一次的唤醒风暴，
     * 又让FIFO水位反映“主机送数 vs 声卡消费”，组件的FIFO_COUNT反馈
     * 才能把主机速率锁到DAC时钟（上游立即抽空FIFO会让主机持续加速）。 */
    s_uac_device->func_id = func_id;
    if (s_uac_device->spk_bytes_per_ms &&
        tud_audio_n_available(func_id) >= (int)s_uac_device->spk_bytes_per_ms) {
        xTaskNotifyGive(s_uac_device->spk_task_handle);
    }
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_sent;
    (void)ep_in;
    (void)cur_alt_setting;
    size_t bytes_require = MIC_INTERVAL_MS * s_uac_device->mic_bytes_per_ms;

    tu_fifo_t *sw_in_fifo = tud_audio_n_get_ep_in_ff(func_id);
    uint16_t fifo_remained = tu_fifo_remaining(sw_in_fifo);
    if (fifo_remained < bytes_require) {
        return true;
    }

    // load data chunk by chunk
    UAC_ENTER_CRITICAL();
    if (s_uac_device->mic_data_size > 0) {
        tud_audio_n_write(func_id, (void *)s_uac_device->mic_buf_read, s_uac_device->mic_data_size);
        s_uac_device->mic_data_size = 0;
    }
    UAC_EXIT_CRITICAL();

    return true;
}

#if SPEAK_CHANNEL_NUM
static void usb_spk_task(void *pvParam)
{
    while (1) {
        /* 本地补丁：反初始化时在安全点退出（不持有任何锁/临界区）。 */
        if (s_uac_device->shutdown) {
            s_uac_device->spk_exited = true;
            vTaskDelete(NULL);
        }
        if (s_uac_device->spk_active == false) {
            ulTaskNotifyTake(pdFAIL, portMAX_DELAY);
            continue;
        }
        /* 等待数据通知；20ms超时用于退出/关流时重新检查状态。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        /* 本地补丁：每次从TinyUSB输出FIFO读1ms交给output_cb。output_cb
         * 在音频ring满时会阻塞（见usb_audio.c的写超时），从而给FIFO施加
         * 背压，让组件自带的FIFO_COUNT反馈把主机速率锁到DAC时钟。 */
        while (s_uac_device->spk_active && !s_uac_device->shutdown) {
            const size_t need = s_uac_device->spk_bytes_per_ms;
            if (!need) break;
            if (tud_audio_n_available(s_uac_device->func_id) < (int)need) break;
            const int got = tud_audio_n_read(s_uac_device->func_id,
                                             s_uac_device->spk_buf, need);
            if (got <= 0) break;
            /* 本地补丁：开流窗口内丢弃数据（主机补发的残留音频）。
             * 只统计含非零样本的块，日志里的时长≈主机真正补发的音频长度，
             * 主机送静音不计入，便于判断窗口是否足够。 */
            if (esp_timer_get_time() < s_uac_device->spk_discard_until_us) {
                for (int i = 0; i < got; ++i) {
                    if (s_uac_device->spk_buf[i]) {
                        s_uac_device->spk_discard_frames +=
                            (uint32_t)(got / (int)SPK_FRAME_BYTES);
                        break;
                    }
                }
                continue;
            }
            /* 窗口结束后报告本次丢弃量，便于确认主机是否补发了残留。 */
            if (!s_uac_device->spk_discard_logged) {
                s_uac_device->spk_discard_logged = true;
                if (s_uac_device->spk_discard_frames &&
                    s_uac_device->current_sample_rate) {
                    ESP_LOGW(TAG, "Discarded %u frames (%u ms) of host audio at stream open",
                             (unsigned)s_uac_device->spk_discard_frames,
                             (unsigned)((uint64_t)s_uac_device->spk_discard_frames * 1000U /
                                        s_uac_device->current_sample_rate));
                }
            }
            if (s_uac_device->user_cfg.output_cb) {
                s_uac_device->user_cfg.output_cb(s_uac_device->spk_buf, (size_t)got,
                                                 s_uac_device->user_cfg.cb_ctx);
            }
        }
    }
}
#endif

#if MIC_CHANNEL_NUM
static void usb_mic_task(void *pvParam)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        if (s_uac_device->mic_active == false) {
            // clear the notification
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            xLastWakeTime = xTaskGetTickCount();
            continue;
        }
        // clear the notification
        // read data from the microphone chunk by chunk
        size_t bytes_require = MIC_INTERVAL_MS * s_uac_device->mic_bytes_per_ms;
        if (s_uac_device->user_cfg.input_cb) {
            size_t bytes_read = 0;
            esp_err_t ret = s_uac_device->user_cfg.input_cb((uint8_t *)s_uac_device->mic_buf_write, bytes_require, &bytes_read, s_uac_device->user_cfg.cb_ctx);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read data from mic");
                continue;
            }
            uint8_t *tmp_buf = s_uac_device->mic_buf_write;
            UAC_ENTER_CRITICAL();
            s_uac_device->mic_buf_write = s_uac_device->mic_buf_read;
            s_uac_device->mic_buf_read = tmp_buf;
            s_uac_device->mic_data_size = bytes_read;
            UAC_EXIT_CRITICAL();
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(MIC_INTERVAL_MS));
    }
}
#endif

/*
 * 本地补丁：初始化中途失败时的完整拆除。
 *
 * 上游在任务创建失败时直接 return，已经启动的 TinyUSB任务/PHY仍会运行；
 * 主机随后 SET_INTERFACE 会调用 tud_audio_set_itf_cb，用空的任务句柄执行
 * xTaskNotifyGive，触发 "assert failed: xTaskGenericNotify (xTaskToNotify)"
 * 并重启。这里按“哪个已创建就删哪个”的顺序收尾，任何一步失败都不会留下
 * 半初始化的USB栈。
 */
static void uac_init_fail_cleanup(void)
{
    if (s_uac_device == NULL) return;
#if SPEAK_CHANNEL_NUM
    if (s_uac_device->spk_task_handle) {
        vTaskDelete(s_uac_device->spk_task_handle);
        s_uac_device->spk_task_handle = NULL;
    }
#endif
#if MIC_CHANNEL_NUM
    if (s_uac_device->mic_task_handle) {
        vTaskDelete(s_uac_device->mic_task_handle);
        s_uac_device->mic_task_handle = NULL;
    }
#endif
    if (s_uac_device->tinyusb_task_handle) {
        vTaskDelete(s_uac_device->tinyusb_task_handle);
        s_uac_device->tinyusb_task_handle = NULL;
    }
    if (tusb_inited()) tusb_deinit(UAC_TINYUSB_RHPORT);
    if (s_uac_device->phy_hdl) {
        usb_del_phy(s_uac_device->phy_hdl);
        s_uac_device->phy_hdl = NULL;
    }
    free(s_uac_device);
    s_uac_device = NULL;
}

esp_err_t uac_device_init(uac_device_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    if (s_uac_device != NULL) {
        ESP_LOGW(TAG, "uac device already initialized");
        return ESP_OK;
    }
    s_uac_device = calloc(1, sizeof(uac_device_t));
    ESP_RETURN_ON_FALSE(s_uac_device != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for uac device");
    s_uac_device->user_cfg.output_cb = config->output_cb;
    s_uac_device->user_cfg.input_cb = config->input_cb;
    s_uac_device->user_cfg.cb_ctx = config->cb_ctx;
    s_uac_device->user_cfg.set_mute_cb = config->set_mute_cb;
    s_uac_device->user_cfg.set_volume_cb = config->set_volume_cb;
    s_uac_device->current_sample_rate = DEFAULT_SAMPLE_RATE;
    s_uac_device->mic_buf_write = s_uac_device->mic_buf1;
    s_uac_device->mic_buf_read = s_uac_device->mic_buf2;

#if CONFIG_USB_DEVICE_UAC_AS_PART
    s_uac_device->spk_itf_num = config->spk_itf_num;
    s_uac_device->mic_itf_num = config->mic_itf_num;
#else
#if SPEAK_CHANNEL_NUM
    s_uac_device->spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK;
#endif
#if MIC_CHANNEL_NUM
    s_uac_device->mic_itf_num = ITF_NUM_AUDIO_STREAMING_MIC;
#endif
#endif

    BaseType_t ret_val;
    if (!config->skip_tinyusb_init) {
        usb_phy_init();
        bool usb_init = tusb_init();
        if (!usb_init) {
            ESP_LOGE(TAG, "USB Device Stack Init Fail");
            uac_init_fail_cleanup();
            return ESP_FAIL;
        }
        ret_val = xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL, CONFIG_UAC_TINYUSB_TASK_PRIORITY,
                                          &s_uac_device->tinyusb_task_handle,
                                          CONFIG_UAC_TINYUSB_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_TINYUSB_TASK_CORE);
        if (ret_val != pdPASS) {
            ESP_LOGE(TAG, "Failed to create TinyUSB task");
            uac_init_fail_cleanup();
            return ESP_FAIL;
        }
    }

#if MIC_CHANNEL_NUM
    ret_val = xTaskCreatePinnedToCore(usb_mic_task, "usb_mic_task", 4096, NULL, CONFIG_UAC_MIC_TASK_PRIORITY,
                                      &s_uac_device->mic_task_handle, CONFIG_UAC_MIC_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_MIC_TASK_CORE);
    if (ret_val != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_mic task");
        uac_init_fail_cleanup();
        return ESP_FAIL;
    }
#endif

#if SPEAK_CHANNEL_NUM
    ret_val = xTaskCreatePinnedToCore(usb_spk_task, "usb_spk_task", 4096, NULL, CONFIG_UAC_SPK_TASK_PRIORITY,
                                      &s_uac_device->spk_task_handle, CONFIG_UAC_SPK_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_SPK_TASK_CORE);
    if (ret_val != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_spk task");
        uac_init_fail_cleanup();
        return ESP_FAIL;
    }
#endif

    ESP_LOGI(TAG, "UAC Device Start, Version: %d.%d.%d", USB_DEVICE_UAC_VER_MAJOR, USB_DEVICE_UAC_VER_MINOR, USB_DEVICE_UAC_VER_PATCH);
    return ESP_OK;
}

/*
 * 本地维护补丁：退出USB声卡模式时释放组件占用的内部RAM（SPK FIFO约6.4KiB
 * +两个4KiB任务栈），否则UAC常驻会挤占SDMMC中转缓冲和本地音乐解码所需
 * 的内部DMA内存。顺序：
 *  1. 撤销D+上拉，主机看到设备断开；
 *  2. 删除两个应用任务，避免它们继续访问即将释放的队列/缓冲；
 *  3. tusb_deinit 关闭DCD中断并释放TinyUSB内部状态；
 *  4. usb_del_phy 关闭PHY并移除中断；
 *  5. 释放结构体，s_uac_device 置空后可以再次 uac_device_init()。
 * 只能在任务上下文调用，且必须与当前UAC会话的USB中断/任务状态一致。
 */
esp_err_t uac_device_deinit(void)
{
    if (s_uac_device == NULL) return ESP_OK;

    if (tusb_inited()) tud_disconnect();

#if SPEAK_CHANNEL_NUM
    /* 通知SPK任务在安全点自行退出：它可能正在output_cb里持有音频临界区，
     * 直接vTaskDelete会有死锁/半更新风险。最多等200ms，超时才强制删除。 */
    if (!s_uac_device->spk_exited) {
        s_uac_device->shutdown = true;
        s_uac_device->spk_active = false;
        if (s_uac_device->spk_task_handle) xTaskNotifyGive(s_uac_device->spk_task_handle);
        for (unsigned i = 0; i < 40 && !s_uac_device->spk_exited; ++i)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (!s_uac_device->spk_exited && s_uac_device->spk_task_handle) {
            /* 超时兜底：任务可能卡在用户回调里，只能强制删除。 */
            vTaskDelete(s_uac_device->spk_task_handle);
        }
        s_uac_device->spk_task_handle = NULL;
    }
#endif
#if MIC_CHANNEL_NUM
    if (s_uac_device->mic_task_handle) {
        vTaskDelete(s_uac_device->mic_task_handle);
        s_uac_device->mic_task_handle = NULL;
    }
#endif
    /* TinyUSB任务阻塞在事件队列上，不持有应用锁，可以直接删除。 */
    if (s_uac_device->tinyusb_task_handle) {
        vTaskDelete(s_uac_device->tinyusb_task_handle);
        s_uac_device->tinyusb_task_handle = NULL;
    }

    if (tusb_inited()) tusb_deinit(UAC_TINYUSB_RHPORT);
    if (s_uac_device->phy_hdl) {
        usb_del_phy(s_uac_device->phy_hdl);
        s_uac_device->phy_hdl = NULL;
    }

    free(s_uac_device);
    s_uac_device = NULL;
    ESP_LOGI(TAG, "UAC device deinitialized");
    return ESP_OK;
}
