/*
 * ESP32-P4 <-> ESP32-U4WDH SDIO link protocol.
 *
 * One packet may be PCM, a command (P4 -> U4) or an event (U4 -> P4).
 * Little-endian. CRC8 is calculated over header bytes type/len/seq/flags
 * (magic and crc excluded) followed by the payload, polynomial 0x07.
 * The SDIO transfer is padded to a 32-bit boundary with zero bytes; header.len
 * and the CRC cover only the real payload, so the receiver accepts 0..3
 * trailing padding bytes.
 *
 * Host -> slave:
 *   type=1 (PCM): interleaved int16 stereo frames, len <= 1024, even length.
 *                 The host must first check LINK_REG_PCM_CREDIT: it may send
 *                 at most credit*256 bytes.
 *   type=2 (CMD): payload[0] = opcode, payload[1..] = arguments.
 *
 * Slave -> host:
 *   type=3 (EVENT): payload[0] = event code, payload[1..] = arguments.
 *                   The slave raises host interrupt 0 after queueing an event.
 *
 * Registers (8-bit, read with CMD52 or ESSL register API):
 *   0 LINK_REG_VERSION      protocol version (1)
 *   1 LINK_REG_BT_STATE     APP_AV_STATE_* from the U4 firmware
 *   2 LINK_REG_MEDIA_STATE  APP_AV_MEDIA_STATE_*
 *   3 LINK_REG_FLAGS        bit0 host present, bit1 test tone enabled
 *   4 LINK_REG_PCM_CREDIT   free PCM ring in 256-byte units (0..255)
 *   5 LINK_REG_AUDIO_RATE   0=44100, 1=48000, 2=other/unknown
 *
 * The PCM sample rate follows the negotiated A2DP SBC rate and is also sent
 * as LINK_EVT_AUDIO_RATE (uint16 little-endian Hz). The P4 must resample its
 * decoded PCM to that rate before sending it.
 */
#ifndef LINK_PROTOCOL_H
#define LINK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LINK_PROTO_VERSION     1
#define LINK_MAGIC             0xA5
#define LINK_HEADER_BYTES      8
#define LINK_MAX_PAYLOAD       1024
#define LINK_MAX_PACKET        (LINK_HEADER_BYTES + LINK_MAX_PAYLOAD)

/* Message types. */
enum {
    LINK_MSG_PCM    = 1,
    LINK_MSG_CMD    = 2,
    LINK_MSG_EVENT  = 3,
};

/* Commands (P4 -> U4). */
enum {
    LINK_CMD_HELLO          = 1, /* no args: host present, disable test tone */
    LINK_CMD_SCAN_START     = 2, /* no args */
    LINK_CMD_SCAN_STOP      = 3, /* no args */
    LINK_CMD_CONNECT        = 4, /* 6-byte Bluetooth address */
    LINK_CMD_DISCONNECT     = 5, /* no args */
    LINK_CMD_VOLUME         = 6, /* 1 byte absolute volume 0..127 */
    LINK_CMD_MEDIA_START    = 7, /* no args */
    LINK_CMD_MEDIA_STOP     = 8, /* no args */
};

/* Events (U4 -> P4). */
enum {
    LINK_EVT_READY          = 1, /* u8 version */
    LINK_EVT_STATE          = 2, /* u8 bt_state, u8 media_state */
    LINK_EVT_SCAN_RESULT    = 3, /* u8 bda[6], i8 rssi, u32 cod(LE),
                                    u8 name_len, char name[name_len] */
    LINK_EVT_CONNECTED      = 4, /* u8 bda[6] */
    LINK_EVT_DISCONNECTED   = 5, /* no args */
    LINK_EVT_MEDIA          = 6, /* u8 media_state */
    LINK_EVT_ERROR          = 7, /* u8 error code */
    LINK_EVT_AUDIO_RATE     = 8, /* u16 sample rate (LE) */
};

#define LINK_REG_VERSION        0
#define LINK_REG_BT_STATE       1
#define LINK_REG_MEDIA_STATE    2
#define LINK_REG_FLAGS          3
#define LINK_REG_PCM_CREDIT     4
#define LINK_REG_AUDIO_RATE     5

#define LINK_FLAG_HOST_PRESENT  0x01
#define LINK_FLAG_TEST_TONE     0x02

#define LINK_AUDIO_RATE_44K     0
#define LINK_AUDIO_RATE_48K     1
#define LINK_AUDIO_RATE_OTHER   2

/* Keep in sync with the A2DP application state machine in main.c. */
enum {
    LINK_BT_IDLE = 0,
    LINK_BT_DISCOVERING,
    LINK_BT_DISCOVERED,
    LINK_BT_UNCONNECTED,
    LINK_BT_CONNECTING,
    LINK_BT_CONNECTED,
    LINK_BT_DISCONNECTING,
};

enum {
    LINK_MEDIA_IDLE = 0,
    LINK_MEDIA_STARTING,
    LINK_MEDIA_STARTED,
    LINK_MEDIA_STOPPING,
};

/* All link structures are packed and little-endian on both MCUs. */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  type;
    uint16_t len;   /* payload length */
    uint16_t seq;
    uint8_t  crc;
    uint8_t  flags;
} link_header_t;

static inline uint8_t link_crc8_byte(uint8_t crc, uint8_t data)
{
    crc ^= data;
    for (unsigned i = 0; i < 8; ++i)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    return crc;
}

static inline uint8_t link_crc_packet(const uint8_t *header, const uint8_t *payload,
                                      size_t payload_len)
{
    uint8_t crc = 0;
    crc = link_crc8_byte(crc, header[1]);
    crc = link_crc8_byte(crc, header[2]);
    crc = link_crc8_byte(crc, header[3]);
    crc = link_crc8_byte(crc, header[4]);
    crc = link_crc8_byte(crc, header[5]);
    crc = link_crc8_byte(crc, header[7]);
    for (size_t i = 0; i < payload_len; ++i)
        crc = link_crc8_byte(crc, payload[i]);
    return crc;
}

#endif /* LINK_PROTOCOL_H */
