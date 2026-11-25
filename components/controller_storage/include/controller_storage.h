#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t controller_storage_init(void);

esp_err_t controller_storage_load_runtime(controller_runtime_t *runtime);
esp_err_t controller_storage_save_runtime(const controller_runtime_t *runtime);

esp_err_t controller_storage_load_preset(uint8_t index,
                                         controller_preset_t *preset);
esp_err_t controller_storage_save_preset(uint8_t index,
                                         const controller_preset_t *preset);

void controller_storage_default_preset(uint8_t index,
                                       controller_preset_t *preset);

/* Independent full scene records: missing key means legacy/no snapshot. */
esp_err_t controller_storage_scene_load(uint8_t preset, uint8_t scene,
                                        controller_scene_profile_t *profile);
esp_err_t controller_storage_scene_save(uint8_t preset, uint8_t scene,
                                        const controller_scene_profile_t *profile);

bool controller_assignment_get(const midi_assignment_t *items,
                               size_t count,
                               uint8_t cc,
                               uint8_t *value);

esp_err_t controller_assignment_set(midi_assignment_t *items,
                                    size_t count,
                                    uint8_t cc,
                                    uint8_t value);

#ifdef __cplusplus
}
#endif
