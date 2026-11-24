#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PARAM_LINEAR = 0,
    /*
     * Piecewise linear calibration through n=0.0, 0.5 and 1.0.
     * This preserves the exact min/mid/max values observed on hardware
     * without prematurely assuming a logarithmic/exponential law.
     */
    NC_PARAM_THREE_POINT = 1,
    NC_PARAM_EXPONENTIAL = 2,
    NC_PARAM_CUBIC = 3,
} nanocore_param_curve_t;

typedef struct {
    const char *name;
    const char *unit;
    float min_value;
    float mid_value;
    float max_value;
    uint8_t decimals;
    nanocore_param_curve_t curve;
} nanocore_param_info_t;

typedef struct {
    uint8_t id;
    const char *name;
    uint8_t expected_param_count;
    const nanocore_param_info_t *params;
    const float *default_normalized;
} nanocore_effect_type_info_t;

float nanocore_spec_normalized_to_real(
    const nanocore_param_info_t *param,
    float normalized);

float nanocore_spec_real_to_normalized(
    const nanocore_param_info_t *param,
    float real_value);

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

/* Physical selector order shown by the current NanoCore firmware. */
size_t nanocore_spec_fx1_selector_count(void);
const nanocore_effect_type_info_t *nanocore_spec_fx1_selector(size_t index);
int nanocore_spec_fx1_selector_index(uint8_t raw_type_id);

/*
 * Stage 06.5 candidate database used by Assisted Mapper.
 * These entries come from captured ToneCommand protocol data and are treated
 * as candidates until the user's physical NanoCore confirms raw id/count.
 */
typedef struct {
    uint8_t id;
    const char *name;
    uint8_t expected_param_count;
    const char *parameter_names;
} nanocore_candidate_type_t;

const nanocore_candidate_type_t *nanocore_spec_candidate_type(
    uint8_t block_id,
    uint8_t raw_type_id);

size_t nanocore_spec_candidate_count(uint8_t block_id);

#ifdef __cplusplus
}
#endif
