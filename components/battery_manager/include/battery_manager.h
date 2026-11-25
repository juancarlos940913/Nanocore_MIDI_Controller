#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t battery_manager_init(void);
/* Main task only. True means a fresh sample is available. */
bool battery_manager_poll(bool *known, uint8_t *percent);
