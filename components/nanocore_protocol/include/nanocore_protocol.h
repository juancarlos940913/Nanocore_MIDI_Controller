#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PROTO_EVENT_NONE = 0,
    NC_PROTO_EVENT_LIVE_STATE,
    NC_PROTO_EVENT_BLOCK_STATE,
    NC_PROTO_EVENT_ACTIVE_SLOT,
    NC_PROTO_EVENT_PRESET_NAME,
} nanocore_protocol_event_type_t;

typedef struct {
    nanocore_protocol_event_type_t type;
    uint8_t slot;
    controller_block_t block;
    bool on;

    bool block_known[BLOCK_COUNT];
    bool block_on[BLOCK_COUNT];

    char name[13];
} nanocore_protocol_event_t;

esp_err_t nanocore_protocol_init(void);
void nanocore_protocol_set_connected(bool connected);

/* Known-safe ToneCommand protocol operations captured from real hardware. */
esp_err_t nanocore_protocol_request_live_state(void);
esp_err_t nanocore_protocol_request_preset_name(uint8_t slot);

/*
 * Rename active preset (8-char device limit) and persist its live state
 * into the same active slot. Returns only after the device ACKs opcode 0x46.
 */
esp_err_t nanocore_protocol_rename_and_save(uint8_t slot,
                                           const char *name,
                                           TickType_t timeout);

/* Feed one complete F0...F7 SysEx message received from BLE-MIDI. */
void nanocore_protocol_on_sysex(const uint8_t *msg, size_t len);

bool nanocore_protocol_get_event(nanocore_protocol_event_t *event,
                                 TickType_t timeout);

#ifdef __cplusplus
}
#endif
