#include "nanocore_spec.h"

static const nanocore_param_info_t FX1_GATE_PARAMS[] = {
    {
        .name = "Threshold",
        .unit = "dB",
        .min_value = -100.0f,
        .max_value = 0.0f,
        .decimals = 0,
        .curve = NC_PARAM_LINEAR,
    },
    {
        .name = "Release",
        .unit = "ms",
        .min_value = 0.0f,
        .max_value = 100.0f,
        .decimals = 1,
        .curve = NC_PARAM_LINEAR,
    },
};

static const nanocore_effect_type_info_t FX1_TYPES[] = {
    { 0,  "Gate",              2, FX1_GATE_PARAMS },
    { 1,  "Auto Gate",         2, 0 },
    { 2,  "Gate + Compressor", 7, 0 },
    { 3,  "Compressor",        6, 0 },
    { 4,  "Env Wah",           4, 0 },
    { 5,  "Wah",               5, 0 },
    { 6,  "Motion Wah",        4, 0 },
    { 7,  "Peak Comp",         5, 0 },
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

    return param->min_value +
           normalized * (param->max_value - param->min_value);
}
