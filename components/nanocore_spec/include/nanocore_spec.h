#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"

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
    NC_PARAM_QUADRATIC = 4,
} nanocore_param_curve_t;

typedef enum {
    NC_PARAM_VALUE_NUMERIC = 0,
    NC_PARAM_VALUE_ENUM = 1,
    NC_PARAM_VALUE_BOOLEAN = 2,
} nanocore_param_value_kind_t;

typedef struct {
    const char *name;
    const char *unit;
    float min_value;
    float mid_value;
    float max_value;
    uint8_t decimals;
    nanocore_param_curve_t curve;

    /* Stage 07 dynamic-editor metadata. Existing Stage 06 entries that omit
     * these fields remain plain numeric parameters with zero default. */
    float default_value;
    nanocore_param_value_kind_t value_kind;
    const char * const *options;
    uint8_t option_count;
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

/* Authoritative 0..29 factory model order used by AMP/CAB saved-preset footer. */
size_t nanocore_spec_amp_model_count(void);
const char *nanocore_spec_amp_model_name(uint8_t model_id);
size_t nanocore_spec_cab_model_count(void);
const char *nanocore_spec_cab_model_name(uint8_t model_id);

/* -------------------------------------------------------------------------
 * Stage 07 generic editor specification
 * ------------------------------------------------------------------------- */

/* Raw type/model lookup and physical selector order. */
size_t nanocore_spec_editor_type_count(controller_block_t block);
const nanocore_effect_type_info_t *nanocore_spec_editor_type_by_raw(
    controller_block_t block,
    uint8_t raw_type_id);
const nanocore_effect_type_info_t *nanocore_spec_editor_type_by_selector(
    controller_block_t block,
    size_t selector_index);
int nanocore_spec_editor_selector_index(
    controller_block_t block,
    uint8_t raw_type_id);

/* Stage 08B Synth voice specs are separate from the eight normal blocks. */
size_t nanocore_spec_synth_voice_count(void);
const nanocore_effect_type_info_t *nanocore_spec_synth_voice(uint8_t voice_id);

/* Stage 08C: mode-aware selector API, separate from normal editor.
 * Synth FX2 remains deliberately unmapped until its physical sweep.
 * Voice lookup uses nanocore_spec_synth_voice(), not a legacy block ID.
 * A zero selector count means no validated selectable types. */
size_t nanocore_spec_mode_type_count(controller_block_t block, bool synth_mode);
const nanocore_effect_type_info_t *nanocore_spec_mode_type_by_selector(
    controller_block_t block, bool synth_mode, size_t index);
int nanocore_spec_mode_selector_index(
    controller_block_t block, bool synth_mode, uint8_t raw_type_id);

/* AMP/CAB use the same type-info shape, but their params are common to every
 * model and the model id comes from 0x41/local live state instead of 0x63. */
bool nanocore_spec_editor_is_model_block(controller_block_t block);

/* Build the normalized parameter vector used when selecting a new effect
 * type. FX1/duplicated FX1 effects preserve captured hardware defaults;
 * other types derive the vector from each param's declared real default. */
bool nanocore_spec_editor_defaults(
    const nanocore_effect_type_info_t *type,
    float *out,
    size_t out_count);

/* UI helpers. */
void nanocore_spec_format_normalized(
    const nanocore_param_info_t *param,
    float normalized,
    char *out,
    size_t out_len);

float nanocore_spec_adjust_normalized(
    const nanocore_param_info_t *param,
    float current,
    int delta);

#ifdef __cplusplus
}
#endif
