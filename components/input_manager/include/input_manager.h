#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_NONE = 0,

    INPUT_ENC1_CLICK,
    INPUT_ENC2_CLICK,

    INPUT_FS_PRESET_DOWN,
    INPUT_FS_PRESET_UP,

    INPUT_FS_MODE_SHORT,
    INPUT_FS_MODE_LONG,

    INPUT_FS_TOP_LEFT,
    INPUT_FS_TOP_RIGHT,

    INPUT_CHORD_SAVE,
    INPUT_CHORD_TUNER,
    INPUT_CHORD_MAPPER,
} input_event_t;

esp_err_t input_manager_init(void);
bool input_manager_get_event(input_event_t *event, TickType_t timeout);

/*
 * Encoder rotation is accumulated outside the button queue so fast turns
 * never build a seconds-long backlog behind full-screen TFT renders.
 * Returns signed detents since the previous call.
 */
int input_manager_take_encoder_delta(uint8_t encoder_index);

#ifdef __cplusplus
}
#endif
