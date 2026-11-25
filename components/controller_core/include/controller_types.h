#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTRL_PRESET_COUNT       64
#define CTRL_SCENE_COUNT         4
#define CTRL_NAME_LEN           24
#define CTRL_BASE_ASSIGNMENTS   24
#define CTRL_SCENE_ASSIGNMENTS  16

typedef enum {
    CTRL_MODE_PRESET = 0,
    CTRL_MODE_STOMP  = 1,
    CTRL_MODE_SCENE  = 2,
    CTRL_MODE_COUNT  = 3,
} controller_mode_t;

typedef enum {
    BLOCK_FX1 = 0,
    BLOCK_FX2,
    BLOCK_AMP,
    BLOCK_CAB,
    BLOCK_DELAY,
    BLOCK_MOD,
    BLOCK_REVERB,
    BLOCK_EQ,
    BLOCK_COUNT,
} controller_block_t;

typedef enum {
    DELAY_SUBDIV_HALF = 0,
    DELAY_SUBDIV_QUARTER,
    DELAY_SUBDIV_EIGHTH,
    DELAY_SUBDIV_SIXTEENTH,
    DELAY_SUBDIV_DOTTED_QUARTER,
    DELAY_SUBDIV_DOTTED_EIGHTH,
    DELAY_SUBDIV_COUNT,
} delay_subdivision_t;

typedef struct {
    uint8_t used;
    uint8_t cc;
    uint8_t value;
} midi_assignment_t;

typedef struct {
    char name[CTRL_NAME_LEN];
    midi_assignment_t assignments[CTRL_SCENE_ASSIGNMENTS];
} controller_scene_t;

typedef struct {
    char name[CTRL_NAME_LEN];
    uint16_t bpm;
    delay_subdivision_t delay_subdivision;
    uint8_t active_scene;
    midi_assignment_t base[CTRL_BASE_ASSIGNMENTS];
    controller_scene_t scene[CTRL_SCENE_COUNT];
} controller_preset_t;

/* Stage 08E scene snapshots live in a separate NVS partition. Never change
 * the layout of controller_scene_t/controller_preset_t (legacy NVS v2). */
#define CTRL_PROFILE_VERSION 1
#define CTRL_PROFILE_PARAMS 9
#define CTRL_PROFILE_SYNTH_PARAMS 8

typedef struct {
    uint8_t on;
    uint8_t type;
    uint8_t count;
    uint8_t reserved;
    float params[CTRL_PROFILE_PARAMS];
} controller_scene_block_t;

typedef struct {
    uint8_t version;
    uint8_t synth_mode;
    uint8_t synth_on;
    uint8_t synth_voice;
    uint8_t synth_count;
    uint8_t reserved[3];
    char name[CTRL_NAME_LEN];
    controller_scene_block_t block[BLOCK_COUNT];
    float synth_params[CTRL_PROFILE_SYNTH_PARAMS];
} controller_scene_profile_t;

typedef struct {
    uint8_t version;
    uint8_t current_preset;
    uint8_t current_scene;
    controller_mode_t mode;
} controller_runtime_t;

#ifdef __cplusplus
}
#endif
