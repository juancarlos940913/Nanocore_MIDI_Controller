#include "nanocore_spec.h"

#define PARAM_LINEAR(_name, _unit, _min, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = ((_min) + (_max)) * 0.5f, \
      .max_value = (_max), .decimals = (_dec), .curve = NC_PARAM_LINEAR }

#define PARAM_3PT(_name, _unit, _min, _mid, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = (_mid), .max_value = (_max), \
      .decimals = (_dec), .curve = NC_PARAM_THREE_POINT }

#define PARAM_UNKNOWN \
    { .name = 0, .unit = 0, .min_value = 0.0f, .mid_value = 0.5f, \
      .max_value = 1.0f, .decimals = 3, .curve = NC_PARAM_LINEAR }

/*
 * Physical calibration source: user's NanoCore firmware, Stage 06 mapper.
 * Values below intentionally follow the physical screen rather than older
 * third-party tables when they disagree.
 */
static const nanocore_param_info_t FX1_GATE_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB", -100.0f,   0.0f, 0),
    PARAM_3PT  ("Release",   "ms",    5.0f,  50.0f, 100.0f, 1),
};

static const nanocore_param_info_t FX1_AUTO_GATE_PARAMS[] = {
    PARAM_LINEAR("Sensitivity", "dB",  -6.0f,   6.0f, 0),
    PARAM_3PT  ("Release",     "ms",   20.0f, 158.0f, 300.0f, 0),
};

static const nanocore_param_info_t FX1_GATE_COMP_PARAMS[] = {
    PARAM_LINEAR("Gate",      "dB", -100.0f,    0.0f, 0),
    PARAM_LINEAR("Threshold", "dB",  -60.0f,    0.0f, 0),
    PARAM_3PT  ("Ratio",      ":1",    1.0f,   10.0f, 20.0f, 1),
    PARAM_LINEAR("Knee",      "dB",    0.0f,   24.0f, 0),
    PARAM_LINEAR("Attack",    "ms",    0.0f,  200.0f, 0),
    PARAM_LINEAR("Release",   "ms",    0.0f, 1000.0f, 0),
    PARAM_LINEAR("Makeup",    "dB",  -24.0f,   24.0f, 0),
};

static const nanocore_param_info_t FX1_COMP_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB",  -60.0f,    0.0f, 0),
    PARAM_3PT  ("Ratio",      ":1",    1.0f,   10.0f, 20.0f, 1),
    PARAM_LINEAR("Knee",      "dB",    0.0f,   24.0f, 0),
    PARAM_LINEAR("Attack",    "ms",    0.0f,  200.0f, 0),
    PARAM_LINEAR("Release",   "ms",    0.0f, 1000.0f, 0),
    PARAM_LINEAR("Makeup",    "dB",  -24.0f,   24.0f, 0),
};

/*
 * Peak Comp reports 5 live parameters. Four have been physically identified
 * so far. p4 remains deliberately unknown until the user identifies the
 * fifth physical control/meaning.
 */
static const nanocore_param_info_t FX1_PEAK_COMP_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB", -60.0f,   0.0f, 0),
    PARAM_3PT  ("Ratio",      ":1",   1.0f,  10.0f, 20.0f, 1),
    PARAM_3PT  ("Attack",     "ms",   0.1f,   4.5f, 200.0f, 1),
    PARAM_3PT  ("Release",    "ms",   1.0f,  40.0f, 2000.0f, 0),
    PARAM_UNKNOWN,
};

static const nanocore_effect_type_info_t FX1_TYPES[] = {
    { 0,  "Gate",              2, FX1_GATE_PARAMS },
    { 1,  "Auto Gate",         2, FX1_AUTO_GATE_PARAMS },
    { 2,  "Gate + Compressor", 7, FX1_GATE_COMP_PARAMS },
    { 3,  "Compressor",        6, FX1_COMP_PARAMS },
    { 4,  "Env Wah",           4, 0 },
    { 5,  "Wah",               5, 0 },
    { 6,  "Motion Wah",        4, 0 },
    { 7,  "Peak Comp",         5, FX1_PEAK_COMP_PARAMS },
    { 8,  "Parallel Comp",     5, 0 },
    { 9,  "Classic Wah",       5, 0 },
    { 10, "Retro Wah",         5, 0 },
    { 11, "Bass Wah",          5, 0 },
};

const nanocore_effect_type_info_t *nanocore_spec_fx1_type(uint8_t type_id)
{
    if (type_id >= (sizeof(FX1_TYPES) / sizeof(FX1_TYPES[0]))) {
        return 0;
    }
    return &FX1_TYPES[type_id];
}

float nanocore_spec_normalized_to_real(
    const nanocore_param_info_t *param,
    float normalized)
{
    if (!param) return normalized;

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    if (param->curve == NC_PARAM_THREE_POINT) {
        if (normalized <= 0.5f) {
            const float t = normalized * 2.0f;
            return param->min_value +
                   t * (param->mid_value - param->min_value);
        }

        const float t = (normalized - 0.5f) * 2.0f;
        return param->mid_value +
               t * (param->max_value - param->mid_value);
    }

    return param->min_value +
           normalized * (param->max_value - param->min_value);
}
