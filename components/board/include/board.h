#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_init(void);
void board_set_tap_led(bool on);
void board_set_mode_led(bool on);
void board_set_backlight(bool on);

#ifdef __cplusplus
}
#endif
