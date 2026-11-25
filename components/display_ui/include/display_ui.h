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

#define UI_MAX_EDITOR_PARAMETERS 14

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

    /* SYNTH is a UI-only selection, never an index into BLOCK_COUNT. */
    bool synth_mode_known;
    bool synth_mode;
    bool synth_selected;
    bool synth_voice_known;
    bool synth_voice_on;
    bool editor_read_only;
    bool scene_busy;
    bool scene_fault;
    bool scene_captured;
    controller_block_t selected_block;
    size_t selected_parameter;
    bool parameter_editing;
    bool parameter_known;
    uint16_t parameter_value;
    const char *parameter_text;

    /*
     * Optional dynamic block-editor model. FX1 uses this so the visible
     * parameter list follows the selected effect/model instead of the old
     * fixed CC table. Delay may append BPM/subdivision utility entries.
     */
    bool editor_override;
    size_t editor_parameter_count;
    const char *editor_parameter_labels[UI_MAX_EDITOR_PARAMETERS];
    const char *selected_parameter_label;

    bool block_known[BLOCK_COUNT];
    bool block_on[BLOCK_COUNT];

    const char *notice;
    const name_editor_t *name_editor;
} display_ui_state_t;

esp_err_t display_ui_init(void);
bool display_ui_is_ready(void);
void display_ui_render(const display_ui_state_t *state);
/* Partial redraw, serialized with normal framebuffer updates. */
void display_ui_update_battery(bool known, uint8_t percent);

#ifdef __cplusplus
}
#endif
