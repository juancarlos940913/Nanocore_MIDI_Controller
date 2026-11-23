#pragma once

#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PARAM_MIDI = 0,
    NC_PARAM_TEMPO_BPM,
    NC_PARAM_TEMPO_SUBDIV,
} nc_param_kind_t;

typedef struct {
    const char *label;
    nc_param_kind_t kind;
    uint8_t cc;
    uint8_t min_value;
    uint8_t max_value;
} nc_parameter_t;

const char *nanocore_block_name(controller_block_t block);
uint8_t nanocore_block_onoff_cc(controller_block_t block);
uint8_t nanocore_block_type_cc(controller_block_t block);
uint8_t nanocore_block_type_max(controller_block_t block);

size_t nanocore_parameter_count(controller_block_t block);
const nc_parameter_t *nanocore_parameter(controller_block_t block,
                                         size_t index);

#ifdef __cplusplus
}
#endif
