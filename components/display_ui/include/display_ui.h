#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MODE_PRESET = 0,
    UI_MODE_STOMP,
    UI_MODE_SCENE,
    UI_MODE_COUNT
} ui_mode_t;

typedef enum {
    UI_BLOCK_UNKNOWN = 0,
    UI_BLOCK_OFF,
    UI_BLOCK_ON,
    UI_BLOCK_PENDING_OFF,
    UI_BLOCK_PENDING_ON
} ui_block_state_t;

typedef struct {
    bool ble_connected;
    bool midi_ready;

    ui_mode_t mode;
    uint8_t preset;
    uint8_t scene; // 0=A, 1=B, 2=C, 3=D

    // Stage 04.3 snapshot amp editor.
    bool amp_editing;
    bool scene_amp_configured;
    uint8_t scene_amp_type;
    uint8_t amp_edit_candidate;

    ui_block_state_t fx1;
    ui_block_state_t fx2;
    ui_block_state_t amp;
    ui_block_state_t cab;
    ui_block_state_t mod;
    ui_block_state_t delay;
    ui_block_state_t reverb;
    ui_block_state_t eq;
} display_ui_state_t;

esp_err_t display_ui_init(void);
bool display_ui_is_ready(void);
void display_ui_render(const display_ui_state_t *state);

#ifdef __cplusplus
}
#endif
