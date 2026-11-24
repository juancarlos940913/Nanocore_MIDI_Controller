#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PARAM_LINEAR = 0,
} nanocore_param_curve_t;

typedef struct {
    const char *name;
    const char *unit;
    float min_value;
    float max_value;
    uint8_t decimals;
    nanocore_param_curve_t curve;
} nanocore_param_info_t;

typedef struct {
    uint8_t id;
    const char *name;
    uint8_t expected_param_count;
    const nanocore_param_info_t *params;
} nanocore_effect_type_info_t;

float nanocore_spec_normalized_to_real(
    const nanocore_param_info_t *param,
    float normalized);

/*
 * FX1 catalog for current NanoCore firmware generation.
 *
 * IDs 0..3 come from the official MIDI/user documentation.
 * IDs 7..11 are the five effects explicitly listed in firmware 1.2 release
 * notes. IDs 4..6 (Env Wah / Wah / Motion Wah) and their wire ids are based
 * on independently captured ToneCommand traffic and are now corroborated by
 * the user's live 0x63 sweep: all 12 ids and parameter counts match.
 */
const nanocore_effect_type_info_t *nanocore_spec_fx1_type(uint8_t type_id);

#ifdef __cplusplus
}
#endif
