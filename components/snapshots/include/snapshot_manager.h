#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNAPSHOT_COUNT 4

typedef struct {
    bool amp_configured;
    uint8_t amp_type_id;
} snapshot_t;

esp_err_t snapshot_manager_init(void);
const snapshot_t *snapshot_manager_get(uint8_t index);
esp_err_t snapshot_manager_set_amp(uint8_t index, uint8_t amp_type_id);
esp_err_t snapshot_manager_clear_amp(uint8_t index);

#ifdef __cplusplus
}
#endif
