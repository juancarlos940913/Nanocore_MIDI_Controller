#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attach the currently discovered BLE-MIDI peer.
 *
 * @param conn_handle BLE connection handle from ble_conn_mgr.
 * @param midi_char_handle Remote BLE-MIDI I/O characteristic value handle.
 */
void nanocore_midi_set_link(uint16_t conn_handle, uint16_t midi_char_handle);

/** Clear the active BLE-MIDI link. */
void nanocore_midi_clear_link(void);

/** True when a BLE connection + remote MIDI characteristic are available. */
bool nanocore_midi_is_ready(void);

/**
 * Send MIDI Program Change over BLE-MIDI.
 *
 * @param channel_1based MIDI channel 1..16.
 * @param program Program number 0..127.
 */
esp_err_t nanocore_midi_send_program_change(uint8_t channel_1based,
                                            uint8_t program);

/**
 * Send MIDI Control Change over BLE-MIDI.
 * Added now so Stage 04 can reuse the same transport without redesign.
 */
esp_err_t nanocore_midi_send_cc(uint8_t channel_1based,
                                uint8_t cc,
                                uint8_t value);

/**
 * Send a complete F0...F7 SysEx message over BLE-MIDI.
 * Long messages are split into ordered BLE-MIDI continuation packets.
 */
esp_err_t nanocore_midi_send_sysex(const uint8_t *sysex, size_t len);

#ifdef __cplusplus
}
#endif
