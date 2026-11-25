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
    { 0, "Gate", 2, FX1_GATE_PARAMS, FX1_DEF_GATE, NULL },
    { 1, "Auto Gate", 2, FX1_AUTO_GATE_PARAMS, FX1_DEF_AUTO_GATE, NULL },
    { 2, "Gate + Compressor", 7, FX1_GATE_COMP_PARAMS, FX1_DEF_GATE_COMP, NULL },
    { 3, "Compressor", 6, FX1_COMP_PARAMS, FX1_DEF_COMP, NULL },
    { 4, "Env Wah", 4, FX1_ENV_WAH_PARAMS, FX1_DEF_ENV_WAH, NULL },
    { 5, "Wah", 5, FX1_WAH_PARAMS, FX1_DEF_WAH, NULL },
    { 6, "Motion Wah", 4, FX1_MOTION_WAH_PARAMS, FX1_DEF_MOTION_WAH, NULL },
    { 7, "Peak Comp", 5, FX1_PEAK_COMP_PARAMS, FX1_DEF_PEAK_COMP, NULL },
    { 8, "Parallel Comp", 5, FX1_PARALLEL_COMP_PARAMS, FX1_DEF_PARALLEL_COMP, NULL },
    { 9, "Classic Wah", 5, FX1_CLASSIC_WAH_PARAMS, FX1_DEF_CLASSIC_WAH, NULL },
    { 10, "Retro Wah", 5, FX1_RETRO_WAH_PARAMS, FX1_DEF_RETRO_WAH, NULL },
    { 11, "Bass Wah", 5, FX1_BASS_WAH_PARAMS, FX1_DEF_BASS_WAH, NULL },
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


/* -------------------------------------------------------------------------
 * Stage 06.5 Assisted Mapper candidate catalogs
 *
 * block_id follows controller_block_t wire order:
 * 0 FX1, 1 FX2, 2 AMP, 3 CAB, 4 DELAY, 5 MOD, 6 REVERB, 7 EQ.
 *
 * AMP/CAB are intentionally omitted here: current 0x63's raw variant byte is
 * not a trustworthy model id for those two blocks. Their common parameter
 * profiles can still be observed, but model identity needs a dedicated read.
 * ------------------------------------------------------------------------- */

static const nanocore_candidate_type_t FX2_CANDIDATES[] = {
    {0,  "Scream",              3, "Gain,Tone,Level"},
    {1,  "Klone",               3, "Gain,Tone,Level"},
    {2,  "OCD",                 3, "Gain,Tone,Level"},
    {3,  "DS2",                 3, "Gain,Tone,Level"},
    {4,  "PIFUZZ",              3, "Gain,Tone,Level"},
    {5,  "Range",               3, "Gain,Tone,Level"},
    {6,  "XAC",                 3, "Gain,Tone,Level"},
    {7,  "Fiman",               3, "Gain,Tone,Level"},
    {8,  "Pitch",               3, "Pitch,Fine,Mix"},
    {9,  "Envelope Wah",        4, "Freq,Env,Q,Mix"},
    {10, "Wah",                 5, "Freq,Sweep,Rate,Shape,Mix"},
    {11, "Motion Wah",          4, "Rate,Voice,Sweep,Mix"},
    {12, "Gate",                2, "Threshold,Release"},
    {13, "Auto Gate",           2, "Sensitivity,Release"},
    {14, "Gate + Compressor",   7, "Gate,Threshold,Ratio,Knee,Attack,Release,Makeup"},
    {15, "Compressor",          6, "Threshold,Ratio,Knee,Attack,Release,Makeup"},
    {16, "Peak Comp",           5, "Threshold,Ratio,Attack,Release,Makeup"},
    {17, "Parallel Comp",       5, "Threshold,Attack,Release,Mix,Level"},
    {18, "Classic Wah",         5, "Position,Low,High,Mix,Level"},
    {19, "Retro Wah",           5, "Position,Low,High,Mix,Level"},
    {20, "Bass Wah",            5, "Position,Q Trim,Volume Trim,Mix,Level"},
};

/*
 * Firmware 1.2 adds Ping Pong to EVERY delay type. Physical 0x63 captures
 * confirm an additional final normalized parameter on the current firmware.
 * Duck therefore grows from 8 to 9 and exercises the protocol maximum.
 */
static const nanocore_candidate_type_t DELAY_CANDIDATES[] = {
    {0, "BBD",       5, "Time,Feedback,Age,Mix,Ping Pong"},
    {1, "Digital",   4, "Time,Feedback,Mix,Ping Pong"},
    {2, "Duck",      9, "Time,Feedback,Sensitivity,Depth,Attack,Release,Filter,Ping Pong,Mix"},
    {3, "Ice",       5, "Time,Feedback,Interval,Mix,Ping Pong"},
    {4, "Reverse",   4, "Time,Feedback,Mix,Ping Pong"},
    {5, "LoFi",      6, "Time,Feedback,Bit Depth,LoFi Mix,Mix,Ping Pong"},
    {6, "Smear",     6, "Time,Feedback,Smear,Filter,Mix,Ping Pong"},
    {7, "Mod Delay", 6, "Time,Feedback,Mod Depth,Mod Rate,Mix,Ping Pong"},
};

static const nanocore_candidate_type_t MOD_CANDIDATES[] = {
    {0,  "Chorus",          2, "Rate,Depth"},
    {1,  "Phaser",          3, "Rate,Feedback,Mix"},
    {2,  "Flanger",         3, "Rate,Feedback,Mix"},
    {3,  "Tremolo",         5, "Rate,Depth,Wave,Smooth,Mix"},
    {4,  "Vibrato",         4, "Rate,Depth,Wave,Mix"},
    {10, "Velvet Vibrato",  5, "Rate,Depth,Wave,Smooth,Mix"},
    {11, "Chorus II",       4, "Rate,Depth,Tone,Mix"},
    {12, "Phaser II",       4, "Rate,Feedback,Tone,Mix"},
    {17, "Jet Flanger",     5, "Rate,Depth,Feedback,Tone,Mix"},
};

/*
 * Physical firmware-1.2 sweep confirms Shimmer at raw id 7 (not legacy
 * CC value/id 4). Room/Plate/Hall/Concert/Cloud/Spring remain unchanged.
 */
static const nanocore_candidate_type_t REVERB_CANDIDATES[] = {
    /* Physical selector order on firmware 1.2. */
    {0, "Room",    5, "Decay,Pre Delay,Tone,Mod,Mix"},
    {1, "Plate",   5, "Decay,Pre Delay,Tone,Mod,Mix"},
    {2, "Hall",    5, "Decay,Pre Delay,Tone,LF Damp,Mix"},
    {3, "Concert", 5, "Decay,Pre Delay,HF Damp,Mod,Mix"},
    {6, "Spring",  5, "Decay,Pre Delay,Crossover,Tone,Mix"},
    {7, "Shimmer", 5, "Decay,Pre Delay,Tone,Pitch,Mix"},
    {5, "Cloud",   5, "Decay,Tone,Mod Depth,Mod Rate,Mix"},
};

static const nanocore_candidate_type_t EQ_CANDIDATES[] = {
    {0, "3-band EQ", 4, "Low,Mid,High,Level"},
    {1, "6-band EQ", 7, "120Hz,250Hz,500Hz,1kHz,2kHz,4kHz,Level"},
    {2, "8-band EQ", 9, "60Hz,120Hz,250Hz,500Hz,1kHz,2kHz,4kHz,8kHz,Level"},
};

static const nanocore_candidate_type_t *candidate_lookup(
    const nanocore_candidate_type_t *items,
    size_t count,
    uint8_t raw_type_id)
{
    for (size_t i = 0; i < count; ++i) {
        if (items[i].id == raw_type_id) return &items[i];
    }
    return 0;
}

const nanocore_candidate_type_t *nanocore_spec_candidate_type(
    uint8_t block_id,
    uint8_t raw_type_id)
{
    switch (block_id) {
    case 0: {
        const nanocore_effect_type_info_t *fx1 =
            nanocore_spec_fx1_type(raw_type_id);
        static nanocore_candidate_type_t converted;
        if (!fx1) return 0;
        converted.id = fx1->id;
        converted.name = fx1->name;
        converted.expected_param_count = fx1->expected_param_count;
        converted.parameter_names = "verified FX1";
        return &converted;
    }
    case 1:
        return candidate_lookup(
            FX2_CANDIDATES,
            sizeof(FX2_CANDIDATES) / sizeof(FX2_CANDIDATES[0]),
            raw_type_id);
    case 4:
        return candidate_lookup(
            DELAY_CANDIDATES,
            sizeof(DELAY_CANDIDATES) / sizeof(DELAY_CANDIDATES[0]),
            raw_type_id);
    case 5:
        return candidate_lookup(
            MOD_CANDIDATES,
            sizeof(MOD_CANDIDATES) / sizeof(MOD_CANDIDATES[0]),
            raw_type_id);
    case 6:
        return candidate_lookup(
            REVERB_CANDIDATES,
            sizeof(REVERB_CANDIDATES) / sizeof(REVERB_CANDIDATES[0]),
            raw_type_id);
    case 7:
        return candidate_lookup(
            EQ_CANDIDATES,
            sizeof(EQ_CANDIDATES) / sizeof(EQ_CANDIDATES[0]),
            raw_type_id);
    default:
        return 0;
    }
}

size_t nanocore_spec_candidate_count(uint8_t block_id)
{
    switch (block_id) {
    case 0: return nanocore_spec_fx1_selector_count();
    case 1: return sizeof(FX2_CANDIDATES) / sizeof(FX2_CANDIDATES[0]);
    case 4: return sizeof(DELAY_CANDIDATES) / sizeof(DELAY_CANDIDATES[0]);
    case 5: return sizeof(MOD_CANDIDATES) / sizeof(MOD_CANDIDATES[0]);
    case 6: return sizeof(REVERB_CANDIDATES) / sizeof(REVERB_CANDIDATES[0]);
    case 7: return sizeof(EQ_CANDIDATES) / sizeof(EQ_CANDIDATES[0]);
    default: return 0;
    }
}


/* -------------------------------------------------------------------------
 * AMP/CAB factory model catalogs.
 *
 * Saved preset opcode 0x41 stores these as direct 0..29 indices. The model
 * order is the same order used by ToneCommand's field 0x06/0x07 selectors.
 * ------------------------------------------------------------------------- */

static const char *AMP_MODEL_NAMES[] = {
    "BogXTC1", "BogXTC2", "ENGP1", "ENGP2", "Fd59Bm",
    "Fd65Dk", "Hiw103", "MarDSL1", "MarDSL2", "Mar8001",
    "Mar8002", "MesR1", "MesR2", "MesR3", "OgG120",
    "Pey51501", "Pey51502", "Pey6505", "RanT21", "RanT22",
    "RoJC120", "SLO1001", "SLO1002", "SLO1003", "VXAC30",
    "ApSVTVR", "ApSVTCL", "Adn900", "Hiw320", "MarSup"
};

static const char *CAB_MODEL_NAMES[] = {
    "Bog412A", "Bog412B", "Eng412A", "Eng412B", "FdTR212",
    "Fd59BM410", "Hiw412", "Mar412A", "Mar412B", "Mar96412A",
    "Mar96412B", "Mes412A", "Mes412B", "Mes412C", "Og412",
    "Pey412A", "Pey412B", "PeyVk412", "Ran112A", "Ran112B",
    "Ran112C", "SLO412A", "SLO412B", "SLO412C", "AC30212",
    "AP810", "Ap410", "Adn410", "HiwB410", "Mar412"
};

size_t nanocore_spec_amp_model_count(void)
{
    return sizeof(AMP_MODEL_NAMES) / sizeof(AMP_MODEL_NAMES[0]);
}

const char *nanocore_spec_amp_model_name(uint8_t model_id)
{
    return model_id < nanocore_spec_amp_model_count()
        ? AMP_MODEL_NAMES[model_id] : 0;
}

size_t nanocore_spec_cab_model_count(void)
{
    return sizeof(CAB_MODEL_NAMES) / sizeof(CAB_MODEL_NAMES[0]);
}

const char *nanocore_spec_cab_model_name(uint8_t model_id)
{
    return model_id < nanocore_spec_cab_model_count()
        ? CAB_MODEL_NAMES[model_id] : 0;
}
