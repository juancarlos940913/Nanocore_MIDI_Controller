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

#define NC_PROTO_MAX_PARAMS       9
#define NC_PROTO_SYNTH_MAX_PARAMS 8
#define NC_PROTO_MODEL_COUNT      30
#define NC_PROTO_MODEL_NAME_LEN   16

typedef enum {
    NC_PROTO_EVENT_NONE = 0,
    NC_PROTO_EVENT_LIVE_STATE,
    NC_PROTO_EVENT_BLOCK_STATE,
    NC_PROTO_EVENT_ACTIVE_SLOT,
    NC_PROTO_EVENT_PRESET_NAME,
    NC_PROTO_EVENT_PRESET_MODELS,
    NC_PROTO_EVENT_PARAM_VALUE,
    NC_PROTO_EVENT_MODEL_CATALOG,
    NC_PROTO_EVENT_SYNTH_CAPTURE,
    NC_PROTO_EVENT_SYNTH_PARAM_VALUE,
} nanocore_protocol_event_type_t;

typedef struct {
    nanocore_protocol_event_type_t type;
    uint8_t slot;
    uint16_t live_seq; /* matched 0x63 reply sequence */
    controller_block_t block;
    bool on;

    bool block_known[BLOCK_COUNT];
    bool block_on[BLOCK_COUNT];

    /*
     * Raw live-profile data from opcode 0x63.
     * variant is the byte reported by the device; for AMP/CAB this byte is
     * not a reliable model index on current firmware, so the mapper keeps it
     * raw rather than silently translating it.
     */
    bool block_profile_known[BLOCK_COUNT];
    uint8_t block_variant[BLOCK_COUNT];
    uint8_t block_param_count[BLOCK_COUNT];
    float block_params[BLOCK_COUNT][NC_PROTO_MAX_PARAMS];

    /* Optional effect-chain footer from the same live profile. */
    bool chain_order_known;
    uint8_t chain_order[BLOCK_COUNT];

    /*
     * Firmware 1.2 Synth Mode is exposed separately (protocol block id 10).
     * Mode flag and voice enable are separate. Stage 08B reads both;
     * no Synth writes or mode-aware editor UI are enabled yet.
     */
    bool synth_mode_known;
    bool synth_mode;
    bool synth_known;
    bool synth_on;
    uint8_t synth_variant;
    uint8_t synth_param_count;
    float synth_params[NC_PROTO_SYNTH_MAX_PARAMS];

    char name[13];

    bool amp_model_known;
    bool cab_model_known;
    uint8_t amp_model;
    uint8_t cab_model;

    uint8_t param_index;
    float param_value;

    /* Read-only AMP/CAB catalog record (0x36 / 0x56). */
    uint16_t catalog_seq;
    uint8_t catalog_status;
    bool model_present;
    char model_name[NC_PROTO_MODEL_NAME_LEN + 1];
} nanocore_protocol_event_t;

esp_err_t nanocore_protocol_init(void);
void nanocore_protocol_set_connected(bool connected);

/* Known-safe ToneCommand protocol operations captured from real hardware. */
esp_err_t nanocore_protocol_request_live_state(void);
esp_err_t nanocore_protocol_request_live_state_seq(uint16_t *seq_out);
esp_err_t nanocore_protocol_set_synth_mode(bool enabled);
esp_err_t nanocore_protocol_set_synth_voice(uint8_t voice, const float *params, uint8_t count);
esp_err_t nanocore_protocol_set_synth_param(uint8_t index, float normalized);

/* Stage 08A: raw, read-only 0x63 capture, isolated from the normal editor.
 * Completion is NC_PROTO_EVENT_SYNTH_CAPTURE with catalog_seq as request id. */
esp_err_t nanocore_protocol_request_synth_capture(uint16_t *seq_out);
/* Main-task only: copy and print the completed capture outside sys_evt. */
bool nanocore_protocol_print_synth_capture(uint16_t seq);
esp_err_t nanocore_protocol_request_preset_name(uint8_t slot);

/*
 * Read the saved-preset footer that contains authoritative AMP/CAB model
 * indices. Unlike live opcode 0x63, these values are the real 0..29 model ids.
 */
esp_err_t nanocore_protocol_request_preset_models(uint8_t slot);

/* Request a single read-only model catalog record. The echoed sequence,
 * block and slot identify its reply; no model data or presets are written. */
/* Read-only AMP/FX2 storage information (0x30). Diagnostic/priming probe. */
esp_err_t nanocore_protocol_request_amp_storage_info(void);

esp_err_t nanocore_protocol_request_model_catalog(
    controller_block_t block, uint8_t model_id, uint16_t *seq_out);

/*
 * Set one block's effect type using the same 0x6D live-edit command family
 * used by ToneCommand. The type change includes the target type's normalized
 * default parameter vector; extended FX1 types are not reliably selectable
 * through the legacy TYPE CC.
 */
esp_err_t nanocore_protocol_set_block_type(
    controller_block_t block,
    uint8_t type_id,
    const float *default_normalized,
    uint8_t param_count);

/* Set one live block parameter by its type-local parameter index. */
esp_err_t nanocore_protocol_set_block_param(
    controller_block_t block,
    uint8_t param_index,
    float normalized_value);

/* AMP/CAB model selection uses scalar 0x6D fields 0x06/0x07, not the
 * generic effect-type payload used by FX1/FX2/DELAY/MOD/REV/EQ. */
esp_err_t nanocore_protocol_set_block_model(
    controller_block_t block,
    uint8_t model_id);

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
