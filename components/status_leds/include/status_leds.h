#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "controller_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_leds_init(controller_mode_t mode, uint16_t bpm);
void status_leds_set_mode(controller_mode_t mode);
void status_leds_set_bpm(uint16_t bpm);
void status_leds_set_tap_capture(bool active);
void status_leds_tap_feedback(void);

#ifdef __cplusplus
}
#endif
