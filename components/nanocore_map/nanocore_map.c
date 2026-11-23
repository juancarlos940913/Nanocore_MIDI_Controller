#include "nanocore_map.h"

#define P_MIDI(label_, cc_, min_, max_) \
    {label_, NC_PARAM_MIDI, cc_, min_, max_}
#define P_BPM(label_) \
    {label_, NC_PARAM_TEMPO_BPM, 0, 30, 300}
#define P_SUB(label_) \
    {label_, NC_PARAM_TEMPO_SUBDIV, 0, 0, DELAY_SUBDIV_COUNT - 1}

static const nc_parameter_t FX1[] = {
    P_MIDI("ON/OFF", 20, 0, 127),
    P_MIDI("TYPE", 40, 0, 3),
    P_MIDI("GATE THR", 50, 0, 127),
    P_MIDI("COMP THR", 51, 0, 127),
    P_MIDI("RATIO", 52, 0, 127),
    P_MIDI("KNEE", 53, 0, 127),
    P_MIDI("ATTACK", 54, 0, 127),
    P_MIDI("RELEASE", 55, 0, 127),
    P_MIDI("MAKEUP", 56, 0, 127),
};

static const nc_parameter_t FX2[] = {
    P_MIDI("ON/OFF", 21, 0, 127),
    P_MIDI("TYPE", 41, 0, 10),
    P_MIDI("GAIN/BOOST", 57, 0, 127),
    P_MIDI("TONE", 58, 0, 127),
    P_MIDI("LEVEL", 59, 0, 127),
    P_MIDI("P1", 68, 0, 127),
    P_MIDI("P2", 69, 0, 127),
    P_MIDI("P3", 70, 0, 127),
    P_MIDI("P4", 71, 0, 127),
    P_MIDI("P5", 72, 0, 127),
    P_MIDI("P6", 73, 0, 127),
};

static const nc_parameter_t AMP[] = {
    P_MIDI("ON/OFF", 23, 0, 127),
    P_MIDI("AMP ID", 43, 0, 127),
    P_MIDI("GAIN", 60, 0, 127),
    P_MIDI("BASS", 61, 0, 127),
    P_MIDI("MID", 62, 0, 127),
    P_MIDI("TREBLE", 63, 0, 127),
    P_MIDI("LEVEL", 64, 0, 127),
};

static const nc_parameter_t CAB[] = {
    P_MIDI("ON/OFF", 24, 0, 127),
    P_MIDI("CAB ID", 44, 0, 127),
    P_MIDI("LOW CUT", 65, 0, 127),
    P_MIDI("HIGH CUT", 66, 0, 127),
    P_MIDI("LEVEL", 67, 0, 127),
};

static const nc_parameter_t MOD[] = {
    P_MIDI("ON/OFF", 25, 0, 127),
    P_MIDI("TYPE", 45, 0, 4),
    P_MIDI("P1", 68, 0, 127),
    P_MIDI("P2", 69, 0, 127),
    P_MIDI("P3", 70, 0, 127),
    P_MIDI("P4", 71, 0, 127),
    P_MIDI("P5", 72, 0, 127),
    P_MIDI("P6", 73, 0, 127),
};

static const nc_parameter_t DELAY[] = {
    P_MIDI("ON/OFF", 26, 0, 127),
    P_MIDI("TYPE", 46, 0, 7),
    P_BPM("TEMPO BPM"),
    P_SUB("NOTE"),
    P_MIDI("FEEDBACK", 75, 0, 127),
    P_MIDI("P3", 76, 0, 127),
    P_MIDI("P4", 77, 0, 127),
    P_MIDI("P5", 78, 0, 127),
    P_MIDI("P6", 79, 0, 127),
};

static const nc_parameter_t REVERB[] = {
    P_MIDI("ON/OFF", 27, 0, 127),
    P_MIDI("TYPE", 47, 0, 6),
    P_MIDI("DECAY", 85, 0, 127),
    P_MIDI("P2", 86, 0, 127),
    P_MIDI("P3", 87, 0, 127),
    P_MIDI("P4", 88, 0, 127),
    P_MIDI("MIX", 89, 0, 127),
    P_MIDI("LEVEL", 90, 0, 127),
};

static const nc_parameter_t EQ[] = {
    P_MIDI("ON/OFF", 28, 0, 127),
    P_MIDI("TYPE", 48, 0, 2),
    P_MIDI("BAND 1", 91, 0, 127),
    P_MIDI("BAND 2", 92, 0, 127),
    P_MIDI("BAND 3", 93, 0, 127),
    P_MIDI("BAND 4", 94, 0, 127),
    P_MIDI("BAND 5", 95, 0, 127),
    P_MIDI("BAND 6", 96, 0, 127),
    P_MIDI("BAND 7", 97, 0, 127),
    P_MIDI("BAND 8", 33, 0, 127),
};

const char *nanocore_block_name(controller_block_t b)
{
    static const char *N[] = {
        "FX1", "FX2", "AMP", "CAB", "MOD", "DELAY", "REVERB", "EQ"
    };
    return b < BLOCK_COUNT ? N[b] : "?";
}

uint8_t nanocore_block_onoff_cc(controller_block_t b)
{
    static const uint8_t C[] = {20,21,23,24,25,26,27,28};
    return b < BLOCK_COUNT ? C[b] : 0;
}

uint8_t nanocore_block_type_cc(controller_block_t b)
{
    static const uint8_t C[] = {40,41,43,44,45,46,47,48};
    return b < BLOCK_COUNT ? C[b] : 0;
}

uint8_t nanocore_block_type_max(controller_block_t b)
{
    static const uint8_t M[] = {3,10,127,127,4,7,6,2};
    return b < BLOCK_COUNT ? M[b] : 127;
}

static const nc_parameter_t *table(controller_block_t b, size_t *count)
{
    switch (b) {
    case BLOCK_FX1: *count = sizeof(FX1)/sizeof(FX1[0]); return FX1;
    case BLOCK_FX2: *count = sizeof(FX2)/sizeof(FX2[0]); return FX2;
    case BLOCK_AMP: *count = sizeof(AMP)/sizeof(AMP[0]); return AMP;
    case BLOCK_CAB: *count = sizeof(CAB)/sizeof(CAB[0]); return CAB;
    case BLOCK_MOD: *count = sizeof(MOD)/sizeof(MOD[0]); return MOD;
    case BLOCK_DELAY: *count = sizeof(DELAY)/sizeof(DELAY[0]); return DELAY;
    case BLOCK_REVERB: *count = sizeof(REVERB)/sizeof(REVERB[0]); return REVERB;
    case BLOCK_EQ: *count = sizeof(EQ)/sizeof(EQ[0]); return EQ;
    default: *count = 0; return 0;
    }
}

size_t nanocore_parameter_count(controller_block_t b)
{
    size_t count = 0;
    (void)table(b, &count);
    return count;
}

const nc_parameter_t *nanocore_parameter(controller_block_t b,
                                         size_t index)
{
    size_t count = 0;
    const nc_parameter_t *t = table(b, &count);
    return (t && index < count) ? &t[index] : 0;
}
