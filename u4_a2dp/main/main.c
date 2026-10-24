/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "link_protocol.h"
#include "sdio_link.h"

/* log tags */
#define BT_AV_TAG             "BT_AV"
#define BT_RC_CT_TAG          "RC_CT"

/* device name */
#define LOCAL_DEVICE_NAME     "ESP_A2DP_SRC"

/* AVRCP used transaction label */
#define APP_RC_CT_TL_GET_CAPS            (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE    (1)
/* SetAbsoluteVolume needs its own AVCTP transaction label: the volume-change
 * notification registered with label 1 can stay outstanding indefinitely. */
#define APP_RC_CT_TL_SET_VOLUME          (2)

enum {
    BT_APP_STACK_UP_EVT   = 0x0000,    /* event for stack up */
    BT_APP_HEART_BEAT_EVT = 0xff00,    /* event for heart beat */
};

/* A2DP global states */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

/* sub states of APP_AV_STATE_CONNECTED */
enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
};

/*********************************
 * STATIC FUNCTION DECLARATIONS
 ********************************/

/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/* avrc controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);

/* GAP callback function */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

/* callback function for A2DP source */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/* callback function for A2DP source audio data stream */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/* handler for heart beat timer */
static void bt_app_a2d_heart_beat(TimerHandle_t arg);

/* A2DP application state machine */
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* utils for transfer BLuetooth Deveice Address into string form */
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param);

/* SDIO link command handling and state publication */
static void bt_av_hdl_link_cmd(uint16_t event, void *p_param);
static void link_cmd_dispatch(const uint8_t *payload, size_t len);
static void notify_link_state(void);
static void bt_app_media_start(void);

/*********************************
 * STATIC VARIABLE DEFINITIONS
 ********************************/

static esp_bd_addr_t s_peer_bda = {0};                        /* Bluetooth Device Address of peer device*/
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];  /* Bluetooth Device Name of peer device*/
static int s_a2d_state = APP_AV_STATE_IDLE;                   /* A2DP global state */
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;           /* sub states of APP_AV_STATE_CONNECTED */
static int s_connecting_intv = 0;                             /* count of heart beat intervals for connecting */
static uint32_t s_pkt_cnt = 0;                                /* count of packets */
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;         /* AVRC target notification event capability bit mask */
static TimerHandle_t s_tmr;                                   /* handle of heart beat timer */

/* Once the P4 has sent any command, the U4 stops the standalone auto-connect
 * behaviour and follows host commands instead. */
static bool s_bt_ready;
static bool s_host_controlled;
/* Standalone mode auto-starts streaming; host-controlled mode starts only on
 * LINK_CMD_MEDIA_START and keeps it stopped after LINK_CMD_MEDIA_STOP. */
static bool s_media_want;

static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;

typedef struct {
    uint8_t opcode;
    uint8_t len;
    uint8_t data[32];
} app_link_cmd_t;

static bool bda_is_zero(const uint8_t bda[6])
{
    for (unsigned i = 0; i < 6; ++i)
        if (bda[i]) return false;
    return true;
}

static void notify_link_state(void)
{
    link_update_bt_state(s_a2d_state, s_media_state);
}

/*********************************
 * STATIC FUNCTION DEFINITIONS
 ********************************/

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    /* get complete or short local name from eir data */
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
    uint32_t cod = 0;     /* class of device */
    int32_t rssi = -129;  /* invalid value */
    uint8_t *eir = NULL;
    esp_bt_gap_dev_prop_t *p;

    /* handle the discovery results */
    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%"PRIx32, cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--RSSI: %"PRId32, rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            if (p->len && p->len <= ESP_BT_GAP_MAX_BDNAME_LEN) {
                memcpy(name, p->val, p->len);
                name[p->len] = '\0';
            }
            break;
        default:
            break;
        }
    }

    if (eir) get_name_from_eir(eir, (uint8_t *)name, NULL);

    /* Only devices with the "rendering" major service class are candidates. */
    if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    /* P4 UI receives every rendering device as a scan result. The link layer
     * itself drops events while no P4 host has been detected. */
    int8_t rssi_out = rssi < -127 ? -127 : (int8_t)rssi;
    link_send_scan_result(param->disc_res.bda, name, rssi_out, cod);

    /* Standalone mode (no P4 command received yet) keeps the original
     * auto-connect to the configured speaker name. */
    if (s_host_controlled) return;
    if (name[0] && strcmp(name, remote_device_name) == 0) {
        ESP_LOGI(BT_AV_TAG, "Found a target device, address %s, name %s", bda_str, name);
        s_a2d_state = APP_AV_STATE_DISCOVERED;
        memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        memcpy(s_peer_bdname, name, sizeof(s_peer_bdname));
        notify_link_state();
        ESP_LOGI(BT_AV_TAG, "Cancel device discovery ...");
        esp_bt_gap_cancel_discovery();
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    /* when device discovered a result, this event comes */
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
            filter_inquiry_scan_result(param);
        }
        break;
    }
    /* when discovery state changed, this event comes */
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
                s_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
                notify_link_state();
                /* connect source to peer device specified by Bluetooth Device Address */
                esp_a2d_source_connect(s_peer_bda);
            } else if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
                if (!s_host_controlled) {
                    /* standalone: keep looking for the configured speaker */
                    ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
                } else {
                    /* host-driven scan finished; report idle and wait */
                    s_a2d_state = APP_AV_STATE_UNCONNECTED;
                    notify_link_state();
                }
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
            notify_link_state();
        }
        break;
    }
    /* when authentication completed, this event comes */
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    }
    /* when Legacy Pairing pin code requested, this event comes */
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit: %d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(BT_AV_TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(BT_AV_TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* when Security Simple Pairing user confirmation requested, this event comes */
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    /* when Security Simple Pairing passkey notified, this event comes */
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06"PRIu32, param->key_notif.passkey);
        break;
    /* when Security Simple Pairing passkey requested, this event comes */
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    /* when GAP mode changed, this event comes */
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
        if (param->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT device name: %s", param->get_dev_name_cmpl.name);
        } else {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT failed, state: %d", param->get_dev_name_cmpl.status);
        }
        break;
    /* other */
    default: {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }

    return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);

    switch (event) {
    /* when stack up worked, this event comes */
    case BT_APP_STACK_UP_EVT: {
        char *dev_name = LOCAL_DEVICE_NAME;
        esp_bt_gap_set_device_name(dev_name);
        esp_bt_gap_register_callback(bt_app_gap_cb);

        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

        esp_avrc_rn_evt_cap_mask_t evt_set = {0};
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

        esp_a2d_source_init();
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);

        /* Avoid the state error of s_a2d_state caused by the connection initiated by the peer device. */
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        esp_bt_gap_get_device_name();

        s_bt_ready = true;
        /* If the P4 already sent HELLO while Bluetooth was starting, do not
         * fall back to the standalone auto-connect mode. */
        if (link_host_present()) s_host_controlled = true;
        link_send_ready(); /* only reaches the P4 after it has said HELLO */

        ESP_LOGI(BT_AV_TAG, "Starting device discovery...");
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        notify_link_state();

        /* create and start heart beat timer */
        do {
            int tmr_id = 0;
            s_tmr = xTimerCreate("connTmr", (10000 / portTICK_PERIOD_MS),
                                 pdTRUE, (void *) &tmr_id, bt_app_a2d_heart_beat);
            xTimerStart(s_tmr, portMAX_DELAY);
        } while (0);
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    /* The negotiated SBC rate decides the PCM rate the P4 must produce. The
     * stack prefers 48 kHz when the sink supports it, otherwise 44.1 kHz. */
    if (event == ESP_A2D_AUDIO_CFG_EVT && param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
        uint8_t sf = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
        uint32_t rate = 48000;
        if (sf & ESP_A2D_SBC_CIE_SF_44K) rate = 44100;
        else if (sf & ESP_A2D_SBC_CIE_SF_48K) rate = 48000;
        ESP_LOGI(BT_AV_TAG, "A2DP SBC rate: %u Hz", (unsigned)rate);
        link_set_audio_rate(rate);
    }
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

/* Pull PCM from the SDIO ring. Until the P4 host has been detected (or when
 * it is paused), play a low-level test tone / silence so the Bluetooth link
 * can be validated before the P4 firmware is ready. */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (data == NULL || len <= 0) {
        return 0;
    }
    size_t got = link_pcm_read(data, (size_t)len);
    if (got < (size_t)len) {
        link_fill_silence_or_tone(data + got, (size_t)len - got);
    }
    return len;
}

static void bt_app_a2d_heart_beat(TimerHandle_t arg)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    ESP_LOGI(BT_AV_TAG, "%s state: %d, event: 0x%x", __func__, s_a2d_state, event);

    /* select handler according to different states */
    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    case APP_AV_STATE_DISCOVERED:
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected_hdlr(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting_hdlr(event, param);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s invalid state: %d", __func__, s_a2d_state);
        break;
    }
}

static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    /* handle the events of interest in unconnected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT: {
        /* In host-controlled mode only the P4 CONNECT command starts a
         * connection; the standalone build keeps the original auto-reconnect. */
        if (s_host_controlled || bda_is_zero(s_peer_bda)) break;
        uint8_t *bda = s_peer_bda;
        ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        esp_a2d_source_connect(s_peer_bda);
        s_a2d_state = APP_AV_STATE_CONNECTING;
        s_connecting_intv = 0;
        notify_link_state();
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connecting state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp connected");
            s_a2d_state =  APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
            /* Standalone auto-plays after connecting; host-controlled waits
             * for LINK_CMD_MEDIA_START. */
            s_media_want = !s_host_controlled;
            link_send_connected(s_peer_bda);
            notify_link_state();
            if (s_media_want) bt_app_media_start();
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
            s_media_want = false;
            link_send_disconnected();
            notify_link_state();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT:
        /**
         * Switch state to APP_AV_STATE_UNCONNECTED
         * when connecting lasts more than 2 heart beat intervals.
         */
        if (++s_connecting_intv >= 2) {
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            s_connecting_intv = 0;
            link_send_error(2); /* connect timeout */
            notify_link_state();
        }
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            if (!s_media_want) break;
            ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS &&
                    s_media_want) {
                ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                s_media_state = APP_AV_MEDIA_STATE_STARTING;
                notify_link_state();
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                if (!s_media_want) {
                    /* Host stopped while the start was in flight. */
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                    s_media_state = APP_AV_MEDIA_STATE_STOPPING;
                } else {
                    ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
                    s_media_state = APP_AV_MEDIA_STATE_STARTED;
                }
            } else {
                /* not started successfully, transfer to idle state */
                ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
            notify_link_state();
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        /* Keep streaming. The P4 controls start/stop through link commands;
         * the original example's 10-heartbeat auto-suspend/disconnect loop is
         * intentionally removed. */
        break;
    }
    case APP_AV_MEDIA_STATE_STOPPING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspended; connection kept");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
                notify_link_state();
                /* A new MEDIA_START can arrive during the suspend. */
                if (s_media_want)
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            } else {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspending...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            }
        }
        break;
    }
    default: {
        break;
    }
    }
}

/* MEDIA_START (host command or standalone auto-start): go through the
 * existing source-ready handshake. */
static void bt_app_media_start(void)
{
    s_media_want = true;
    if (s_a2d_state != APP_AV_STATE_CONNECTED) return;
    if (s_media_state == APP_AV_MEDIA_STATE_IDLE)
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
}

static void bt_app_av_state_connected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
            s_media_want = false;
            link_send_disconnected();
            notify_link_state();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not supposed to occur for A2DP source
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT: {
        bt_app_av_media_proc(event, param);
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in disconnecing state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
            s_media_want = false;
            link_send_disconnected();
            notify_link_state();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: 0x%u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_METADATA_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    }
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
    }
}

static void bt_av_volume_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    /* when volume changed locally on target, this event comes */
    case ESP_AVRC_RN_VOLUME_CHANGE: {
        /* 远端（耳机/音箱）自己调整了音量：只记录并重新注册通知，
         * 不能回发 volume+5。回发会与 P4 的音量意图互相覆盖，且原例程
         * 的 +5 是演示值，容易造成音量跳变/协议事务标签冲突。 */
        ESP_LOGI(BT_RC_CT_TAG, "Remote volume changed: %d", event_parameter->volume);
        bt_av_volume_changed();
        break;
    }
    /* other */
    default:
        break;
    }
}

/* AVRC controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_RC_CT_TAG, "%s evt %d", __func__, event);
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);

    switch (event) {
    /* when connection state changed, this event comes */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state event: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
        } else {
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    /* when passthrough responded, this event comes */
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough response: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code,
                    rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    }
    /* when metadata responded, this event comes */
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC metadata response: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
        free(rc->meta_rsp.attr_text);
        break;
    }
    /* when notification changed, this event comes */
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    /* when indicate feature of remote device, this event comes */
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %"PRIx32", TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    /* when get supported notification events capability of peer device, this event comes */
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;

        bt_av_volume_changed();
        break;
    }
    /* when set absolute volume responded, this event comes */
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume response: volume %d", rc->set_volume_rsp.volume);
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/*********************************
 * SDIO LINK COMMANDS
 ********************************/

static void link_cmd_dispatch(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0 || len - 1 > sizeof(((app_link_cmd_t *)0)->data)) return;
    if (!s_bt_ready) {
        link_send_error(1);
        return;
    }
    app_link_cmd_t cmd = {0};
    cmd.opcode = payload[0];
    cmd.len = (uint8_t)(len - 1);
    memcpy(cmd.data, payload + 1, cmd.len);
    if (!bt_app_work_dispatch(bt_av_hdl_link_cmd, 0, &cmd, sizeof(cmd), NULL))
        link_send_error(5); /* command queue full */
}

static void bt_av_hdl_link_cmd(uint16_t event, void *p_param)
{
    (void)event;
    const app_link_cmd_t *cmd = (const app_link_cmd_t *)p_param;
    if (!cmd) return;
    if (!s_bt_ready) {
        link_send_error(1);
        return;
    }

    /* Any valid host command takes precedence over the standalone auto-connect
     * behaviour, even if HELLO was lost. */
    s_host_controlled = true;

    switch (cmd->opcode) {
    case LINK_CMD_HELLO:
        s_host_controlled = true;
        link_send_ready();
        notify_link_state();
        break;
    case LINK_CMD_SCAN_START:
        s_host_controlled = true;
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) break;
        if (s_a2d_state != APP_AV_STATE_CONNECTED &&
            s_a2d_state != APP_AV_STATE_CONNECTING &&
            s_a2d_state != APP_AV_STATE_DISCONNECTING) {
            s_a2d_state = APP_AV_STATE_DISCOVERING;
            ESP_LOGI(BT_AV_TAG, "Host requested discovery");
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            notify_link_state();
        }
        break;
    case LINK_CMD_SCAN_STOP:
        s_host_controlled = true;
        if (s_a2d_state == APP_AV_STATE_DISCOVERING)
            esp_bt_gap_cancel_discovery();
        break;
    case LINK_CMD_CONNECT:
        if (cmd->len != 6) {
            link_send_error(2);
            break;
        }
        s_host_controlled = true;
        if (s_a2d_state == APP_AV_STATE_CONNECTED ||
            s_a2d_state == APP_AV_STATE_CONNECTING ||
            s_a2d_state == APP_AV_STATE_DISCONNECTING) {
            link_send_error(3); /* busy */
            break;
        }
        if (s_a2d_state == APP_AV_STATE_DISCOVERING)
            esp_bt_gap_cancel_discovery();
        memcpy(s_peer_bda, cmd->data, 6);
        s_peer_bdname[0] = '\0';
        s_a2d_state = APP_AV_STATE_CONNECTING;
        s_media_want = false; /* wait for MEDIA_START */
        s_connecting_intv = 0;
        ESP_LOGI(BT_AV_TAG, "Host requested connect to %02x:%02x:%02x:%02x:%02x:%02x",
                 s_peer_bda[0], s_peer_bda[1], s_peer_bda[2],
                 s_peer_bda[3], s_peer_bda[4], s_peer_bda[5]);
        esp_a2d_source_connect(s_peer_bda);
        notify_link_state();
        break;
    case LINK_CMD_DISCONNECT:
        s_host_controlled = true;
        s_media_want = false;
        if (s_a2d_state == APP_AV_STATE_CONNECTED ||
            s_a2d_state == APP_AV_STATE_CONNECTING) {
            esp_a2d_source_disconnect(s_peer_bda);
            s_a2d_state = APP_AV_STATE_DISCONNECTING;
            notify_link_state();
        }
        break;
    case LINK_CMD_FORGET:
        if (cmd->len != 6) {
            link_send_error(2);
            break;
        }
        s_host_controlled = true;
        if (memcmp(s_peer_bda, cmd->data, 6) == 0 &&
            (s_a2d_state == APP_AV_STATE_CONNECTED ||
             s_a2d_state == APP_AV_STATE_CONNECTING ||
             s_a2d_state == APP_AV_STATE_DISCONNECTING)) {
            s_media_want = false;
            esp_a2d_source_disconnect(s_peer_bda);
            s_a2d_state = APP_AV_STATE_DISCONNECTING;
            notify_link_state();
        }
        {
            /* API 需要可写 esp_bd_addr_t；命令缓冲区是 const，复制一份。 */
            esp_bd_addr_t bond;
            memcpy(bond, cmd->data, sizeof(bond));
            esp_err_t err = esp_bt_gap_remove_bond_device(bond);
            ESP_LOGI(BT_AV_TAG, "Remove bond %02x:%02x:%02x:%02x:%02x:%02x: %s",
                     bond[0], bond[1], bond[2], bond[3], bond[4], bond[5],
                     esp_err_to_name(err));
        }
        break;
    case LINK_CMD_VOLUME:
        if (cmd->len != 1 || cmd->data[0] > 127) {
            link_send_error(6);
            break;
        }
        {
            esp_err_t vol_err = esp_avrc_ct_send_set_absolute_volume_cmd(
                APP_RC_CT_TL_SET_VOLUME, cmd->data[0]);
            if (vol_err != ESP_OK)
                ESP_LOGW(BT_RC_CT_TAG, "set absolute volume %u failed: %s",
                         (unsigned)cmd->data[0], esp_err_to_name(vol_err));
        }
        break;
    case LINK_CMD_MEDIA_START:
        bt_app_media_start();
        break;
    case LINK_CMD_MEDIA_STOP:
        s_media_want = false;
        if (s_a2d_state == APP_AV_STATE_CONNECTED &&
            s_media_state == APP_AV_MEDIA_STATE_STARTED) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            s_media_state = APP_AV_MEDIA_STATE_STOPPING;
            notify_link_state();
        }
        /* If a start is still in flight, its ACK path sees s_media_want==false
         * and suspends the stream. */
        break;
    default:
        link_send_error(4);
        break;
    }
}

/*********************************
 * MAIN ENTRY POINT
 ********************************/

void app_main(void)
{
    char bda_str[18] = {0};
    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Allocate the SDIO PCM ring before the Bluetooth stack fragments internal
     * RAM. Commands that arrive before the stack is ready are rejected and can
     * be retried by the P4. */
    link_init(link_cmd_dispatch);

    /*
     * This example only uses the functions of Classical Bluetooth.
     * So release the controller memory for Bluetooth Low Energy.
     */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed", __func__);
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed", __func__);
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed", __func__);
        return;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(BT_AV_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    bt_app_task_start_up();
    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_STACK_UP_EVT, NULL, 0, NULL);
    ESP_LOGI(BT_AV_TAG, "Free heap after init: %"PRIu32" bytes",
             (uint32_t)esp_get_free_heap_size());
}
