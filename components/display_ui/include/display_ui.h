#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"
#include "name_editor.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_PERFORMANCE = 0,
    UI_SCREEN_BLOCK_EDITOR,
    UI_SCREEN_NAME_EDITOR,
    UI_SCREEN_TUNER,
} ui_screen_t;

typedef struct {
    bool ble_connected;
    bool midi_ready;
    bool dirty;
    bool tap_capture;

    ui_screen_t screen;
    controller_mode_t mode;
    uint8_t preset_index;
    uint8_t scene_index;
    uint16_t bpm;
    delay_subdivision_t subdivision;

    const controller_preset_t *preset;

    controller_block_t selected_block;
    size_t selected_parameter;
    bool parameter_editing;
    bool parameter_known;
    uint16_t parameter_value;

    const name_editor_t *name_editor;
} display_ui_state_t;

esp_err_t display_ui_init(void);
bool display_ui_is_ready(void);
void display_ui_render(const display_ui_state_t *state);

#ifdef __cplusplus
}
#endif
