#include "nanocore_spec.h"

static const nanocore_effect_type_info_t FX1_TYPES[] = {
    { 0,  "Gate",              2 },
    { 1,  "Auto Gate",         2 },
    { 2,  "Gate + Compressor", 7 },
    { 3,  "Compressor",        6 },
    { 4,  "Env Wah",           4 },
    { 5,  "Wah",               5 },
    { 6,  "Motion Wah",        4 },
    { 7,  "Peak Comp",         5 },
    { 8,  "Parallel Comp",     5 },
    { 9,  "Classic Wah",       5 },
    { 10, "Retro Wah",         5 },
    { 11, "Bass Wah",          5 },
};

const nanocore_effect_type_info_t *nanocore_spec_fx1_type(uint8_t type_id)
{
    if (type_id >= (sizeof(FX1_TYPES) / sizeof(FX1_TYPES[0]))) {
        return 0;
    }
    return &FX1_TYPES[type_id];
}
