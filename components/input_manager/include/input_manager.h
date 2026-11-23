#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_NONE = 0,

    INPUT_ENC1_LEFT,
    INPUT_ENC1_RIGHT,
    INPUT_ENC1_CLICK,

    INPUT_ENC2_LEFT,
    INPUT_ENC2_RIGHT,
    INPUT_ENC2_CLICK,

    INPUT_FS_PRESET_DOWN,
    INPUT_FS_PRESET_UP,

    INPUT_FS_MODE_SHORT,
    INPUT_FS_MODE_LONG,

    INPUT_FS_TOP_LEFT,
    INPUT_FS_TOP_RIGHT,

    INPUT_CHORD_SAVE,
    INPUT_CHORD_TUNER,
} input_event_t;

esp_err_t input_manager_init(void);
bool input_manager_get_event(input_event_t *event, TickType_t timeout);

#ifdef __cplusplus
}
#endif
