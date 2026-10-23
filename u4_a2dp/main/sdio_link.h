/*
 * SDIO slave transport for the ESP32-U4WDH Bluetooth coprocessor.
 *
 * The SDIO receive task writes PCM into a 32 KiB internal-RAM ring; the A2DP
 * source data callback consumes it. Any valid packet from the P4 marks the
 * host as present, disables the standalone test tone and updates the flags
 * register. Commands are dispatched to the Bluetooth application task by the
 * caller-provided callback (never call Bluedroid from the SDIO task directly).
 *
 * The link is optional: if SDIO initialization fails, Bluetooth keeps running
 * with the test tone so the board can still be validated.
 */
#ifndef SDIO_LINK_H
#define SDIO_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the SDIO receive task. The pointer is only valid during the
 * call; the callback must copy what it needs. */
typedef void (*link_cmd_cb_t)(const uint8_t *payload, size_t len);

/* Initialize the SDIO slave, ring and tasks. Safe to call once at boot. */
void link_init(link_cmd_cb_t cmd_cb);

/* Pop up to bytes from the PCM ring. Returns the number of bytes written. */
size_t link_pcm_read(void *dst, size_t bytes);

/* Fill the remainder of an A2DP data buffer: a 440 Hz test tone until a real
 * host has been seen, otherwise silence. */
void link_fill_silence_or_tone(uint8_t *dst, size_t bytes);

bool link_host_present(void);

/* Publish state to registers and, when a host is connected, as an event. */
void link_update_bt_state(int bt_state, int media_state);
void link_send_ready(void);
void link_send_scan_result(const uint8_t bda[6], const char *name,
                           int8_t rssi, uint32_t cod);
void link_send_connected(const uint8_t bda[6]);
void link_send_disconnected(void);
void link_send_error(uint8_t code);

/* Update the negotiated SBC sample rate; resets the test-tone phase. */
void link_set_audio_rate(uint32_t rate);

#ifdef __cplusplus
}
#endif

#endif /* SDIO_LINK_H */
