#include "nanocore_spec.h"

#include <math.h>

#define PARAM_LINEAR(_name, _unit, _min, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = ((_min) + (_max)) * 0.5f, \
      .max_value = (_max), .decimals = (_dec), .curve = NC_PARAM_LINEAR }

#define PARAM_3PT(_name, _unit, _min, _mid, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = (_mid), .max_value = (_max), \
      .decimals = (_dec), .curve = NC_PARAM_THREE_POINT }

#define PARAM_EXP(_name, _unit, _min, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = 0.0f, .max_value = (_max), \
      .decimals = (_dec), .curve = NC_PARAM_EXPONENTIAL }

#define PARAM_CUBIC(_name, _unit, _min, _max, _dec) \
    { .name = (_name), .unit = (_unit), \
      .min_value = (_min), .mid_value = 0.0f, .max_value = (_max), \
      .decimals = (_dec), .curve = NC_PARAM_CUBIC }

/*
 * FX1 physical specification.
 *
 * Primary source: user's real NanoCore screen + Stage 06 opcode-0x63 sweeps.
 * The handwritten "same as above" marks are expanded here to the most recent
 * fully specified parameter with the same name.
 *
 * IDs are the raw 0x63 variant ids, NOT the physical selector order:
 * selector 1..12 -> raw ids 0,1,2,3,7,8,4,5,6,9,10,11.
 */

static const nanocore_param_info_t FX1_GATE_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB", -100.0f, 0.0f, 0),
    PARAM_LINEAR("Release", "ms", 5.0f, 100.0f, 1),
};

static const nanocore_param_info_t FX1_AUTO_GATE_PARAMS[] = {
    PARAM_LINEAR("Sensitivity", "dB", -6.0f, 6.0f, 0),
    PARAM_LINEAR("Release", "ms", 20.0f, 300.0f, 1),
};

static const nanocore_param_info_t FX1_GATE_COMP_PARAMS[] = {
    PARAM_LINEAR("Gate",      "dB", -100.0f,    0.0f, 0),
    PARAM_LINEAR("Threshold", "dB",  -60.0f,    0.0f, 0),
    PARAM_LINEAR("Ratio",     ":1",    1.0f,   20.0f, 1),
    PARAM_LINEAR("Knee",      "dB",    0.0f,   24.0f, 1),
    PARAM_LINEAR("Attack",    "ms",    0.0f,  200.0f, 1),
    PARAM_LINEAR("Release",   "ms",    0.0f, 1000.0f, 1),
    PARAM_LINEAR("Makeup",    "dB",  -24.0f,   24.0f, 0),
};

static const nanocore_param_info_t FX1_COMP_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB",  -60.0f,    0.0f, 0),
    PARAM_LINEAR("Ratio",     ":1",    1.0f,   20.0f, 1),
    PARAM_LINEAR("Knee",      "dB",    0.0f,   24.0f, 1),
    PARAM_LINEAR("Attack",    "ms",    0.0f,  200.0f, 1),
    PARAM_LINEAR("Release",   "ms",    0.0f, 1000.0f, 1),
    PARAM_LINEAR("Makeup",    "dB",  -24.0f,   24.0f, 0),
};

static const nanocore_param_info_t FX1_ENV_WAH_PARAMS[] = {
    PARAM_EXP   ("Freq", "Hz", 10.0f, 20000.0f, 0),
    PARAM_LINEAR("Env",  "Hz",  0.0f, 20000.0f, 0),
    PARAM_LINEAR("Q",    "",    0.1f,   20.0f, 1),
    PARAM_LINEAR("Mix",  "",    0.0f,   100.0f, 0),
};

static const nanocore_param_info_t FX1_WAH_PARAMS[] = {
    PARAM_EXP   ("Freq",  "Hz", 163.0f, 3500.0f, 0),
    PARAM_LINEAR("Sweep", "",     0.0f,  100.0f, 0),
    PARAM_LINEAR("Rate",  "Hz",   0.0f,    8.0f, 1),
    PARAM_LINEAR("Shape", "",     0.0f,  100.0f, 0),
    PARAM_LINEAR("Mix",   "",     0.0f,  100.0f, 0),
};

static const nanocore_param_info_t FX1_MOTION_WAH_PARAMS[] = {
    PARAM_LINEAR("Rate",  "Hz", 0.0f,  15.0f, 1),
    PARAM_LINEAR("Voice", "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Sweep", "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Mix",   "",   0.0f, 100.0f, 0),
};

static const nanocore_param_info_t FX1_PEAK_COMP_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB", -60.0f,   0.0f, 0),
    PARAM_LINEAR("Ratio",     ":1",   1.0f,  20.0f, 1),
    PARAM_EXP   ("Attack",    "ms",   0.1f, 200.0f, 1),
    PARAM_EXP   ("Release",   "ms",   1.0f, 2000.0f, 1),
    PARAM_LINEAR("Makeup",    "dB", -24.0f,  24.0f, 0),
};

static const nanocore_param_info_t FX1_PARALLEL_COMP_PARAMS[] = {
    PARAM_LINEAR("Threshold", "dB", -60.0f,    0.0f, 0),
    PARAM_CUBIC ("Attack",    "ms",   0.01f, 250.0f, 2),
    PARAM_CUBIC ("Release",   "ms",  10.0f, 2500.0f, 1),
    PARAM_LINEAR("Mix",       "",     0.0f,  200.0f, 0),
    PARAM_LINEAR("Level",     "dB", -24.0f,   24.0f, 0),
};

static const nanocore_param_info_t FX1_CLASSIC_WAH_PARAMS[] = {
    PARAM_LINEAR("Position", "",    0.0f, 100.0f, 0),
    PARAM_LINEAR("Low",      "Hz", 20.0f, 500.0f, 0),
    PARAM_LINEAR("High",     "Hz", 500.0f, 5000.0f, 0),
    PARAM_LINEAR("Mix",      "",    0.0f, 100.0f, 0),
    PARAM_LINEAR("Level",    "dB",-60.0f, 6.0f, 0),
};

static const nanocore_param_info_t FX1_RETRO_WAH_PARAMS[] = {
    PARAM_LINEAR("Position", "",    0.0f, 100.0f, 0),
    PARAM_LINEAR("Low",      "Hz", 20.0f, 500.0f, 0),
    PARAM_LINEAR("High",     "Hz", 500.0f, 5000.0f, 0),
    PARAM_LINEAR("Mix",      "",    0.0f, 100.0f, 0),
    PARAM_LINEAR("Level",    "dB",-60.0f, 6.0f, 0),
};

static const nanocore_param_info_t FX1_BASS_WAH_PARAMS[] = {
    PARAM_LINEAR("Position",    "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Q Trim",      "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Volume Trim", "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Mix",         "",   0.0f, 100.0f, 0),
    PARAM_LINEAR("Level",       "dB",-60.0f, 6.0f, 0),
};

/*
 * Default normalized values captured immediately after selecting each type on
 * the physical NanoCore during Stage 06.  Bass Wah uses the independently
 * confirmed factory defaults because the long sweep exhausted BLE before the
 * second variant-11 snapshot.
 */
static const float FX1_DEF_GATE[]      = {0.20f, 0.473684f};
static const float FX1_DEF_AUTO_GATE[] = {0.50f, 0.321429f};
static const float FX1_DEF_GATE_COMP[] = {
    0.20f, 0.666667f, 0.157895f, 0.00f, 0.05f, 0.10f, 0.50f
};
static const float FX1_DEF_COMP[] = {
    0.666667f, 0.157895f, 0.00f, 0.05f, 0.10f, 0.50f
};
static const float FX1_DEF_ENV_WAH[] = {
    0.514679f, 0.05f, 0.396985f, 0.80f
};
static const float FX1_DEF_WAH[] = {
    0.50f, 0.50f, 0.1875f, 0.00f, 0.80f
};
static const float FX1_DEF_MOTION_WAH[] = {
    0.14f, 0.50f, 0.55f, 1.00f
};
static const float FX1_DEF_PEAK_COMP[] = {
    0.90f, 0.052632f, 0.715452f, 0.412516f, 0.50f
};
static const float FX1_DEF_PARALLEL_COMP[] = {
    0.80f, 0.206456f, 0.417139f, 1.00f, 0.50f
};
static const float FX1_DEF_CLASSIC_WAH[] = {
    0.00f, 0.90625f, 0.367778f, 1.00f, 0.909091f
};
static const float FX1_DEF_RETRO_WAH[] = {
    0.00f, 0.864583f, 0.311333f, 1.00f, 0.909091f
};
static const float FX1_DEF_BASS_WAH[] = {
    0.00f, 1.00f, 0.70f, 1.00f, 0.909091f
};

static const nanocore_effect_type_info_t FX1_TYPES[] = {
    { 0,  "Gate",              2, FX1_GATE_PARAMS,          FX1_DEF_GATE },
    { 1,  "Auto Gate",         2, FX1_AUTO_GATE_PARAMS,     FX1_DEF_AUTO_GATE },
    { 2,  "Gate + Compressor", 7, FX1_GATE_COMP_PARAMS,     FX1_DEF_GATE_COMP },
    { 3,  "Compressor",        6, FX1_COMP_PARAMS,          FX1_DEF_COMP },
    { 4,  "Env Wah",           4, FX1_ENV_WAH_PARAMS,       FX1_DEF_ENV_WAH },
    { 5,  "Wah",               5, FX1_WAH_PARAMS,           FX1_DEF_WAH },
    { 6,  "Motion Wah",        4, FX1_MOTION_WAH_PARAMS,    FX1_DEF_MOTION_WAH },
    { 7,  "Peak Comp",         5, FX1_PEAK_COMP_PARAMS,     FX1_DEF_PEAK_COMP },
    { 8,  "Parallel Comp",     5, FX1_PARALLEL_COMP_PARAMS, FX1_DEF_PARALLEL_COMP },
    { 9,  "Classic Wah",       5, FX1_CLASSIC_WAH_PARAMS,   FX1_DEF_CLASSIC_WAH },
    { 10, "Retro Wah",         5, FX1_RETRO_WAH_PARAMS,     FX1_DEF_RETRO_WAH },
    { 11, "Bass Wah",          5, FX1_BASS_WAH_PARAMS,      FX1_DEF_BASS_WAH },
};

static const uint8_t FX1_SELECTOR_ORDER[] = {
    0, 1, 2, 3, 7, 8, 4, 5, 6, 9, 10, 11
};

const nanocore_effect_type_info_t *nanocore_spec_fx1_type(uint8_t type_id)
{
    if (type_id >= (sizeof(FX1_TYPES) / sizeof(FX1_TYPES[0]))) {
        return 0;
    }
    return &FX1_TYPES[type_id];
}

size_t nanocore_spec_fx1_selector_count(void)
{
    return sizeof(FX1_SELECTOR_ORDER) / sizeof(FX1_SELECTOR_ORDER[0]);
}

const nanocore_effect_type_info_t *nanocore_spec_fx1_selector(size_t index)
{
    if (index >= nanocore_spec_fx1_selector_count()) return 0;
    return nanocore_spec_fx1_type(FX1_SELECTOR_ORDER[index]);
}

int nanocore_spec_fx1_selector_index(uint8_t raw_type_id)
{
    for (size_t i = 0; i < nanocore_spec_fx1_selector_count(); ++i) {
        if (FX1_SELECTOR_ORDER[i] == raw_type_id) return (int)i;
    }
    return -1;
}

float nanocore_spec_normalized_to_real(
    const nanocore_param_info_t *param,
    float normalized)
{
    if (!param) return normalized;

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    if (param->curve == NC_PARAM_EXPONENTIAL &&
        param->min_value > 0.0f &&
        param->max_value > param->min_value) {
        return param->min_value *
               powf(param->max_value / param->min_value, normalized);
    }

    if (param->curve == NC_PARAM_CUBIC) {
        const float n2 = normalized * normalized;
        return param->min_value +
               (param->max_value - param->min_value) *
               n2 * normalized;
    }

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

float nanocore_spec_real_to_normalized(
    const nanocore_param_info_t *param,
    float real_value)
{
    if (!param) return real_value;

    if (real_value <= param->min_value) return 0.0f;
    if (real_value >= param->max_value) return 1.0f;

    if (param->curve == NC_PARAM_EXPONENTIAL &&
        param->min_value > 0.0f &&
        param->max_value > param->min_value) {
        return logf(real_value / param->min_value) /
               logf(param->max_value / param->min_value);
    }

    if (param->curve == NC_PARAM_CUBIC) {
        const float span = param->max_value - param->min_value;
        if (span <= 0.0f) return 0.0f;
        const float t = (real_value - param->min_value) / span;
        return cbrtf(t);
    }

    if (param->curve == NC_PARAM_THREE_POINT) {
        if (real_value <= param->mid_value) {
            const float span = param->mid_value - param->min_value;
            if (span <= 0.0f) return 0.0f;
            return 0.5f *
                   (real_value - param->min_value) / span;
        }

        const float span = param->max_value - param->mid_value;
        if (span <= 0.0f) return 1.0f;
        return 0.5f +
               0.5f * (real_value - param->mid_value) / span;
    }

    const float span = param->max_value - param->min_value;
    if (span <= 0.0f) return 0.0f;
    return (real_value - param->min_value) / span;
}
