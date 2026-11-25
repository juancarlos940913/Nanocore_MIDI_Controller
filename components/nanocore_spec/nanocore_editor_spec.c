#include "nanocore_spec.h"

#include <math.h>
#include <string.h>

#define P_LINEAR(_name,_unit,_min,_max,_dec) \
    { .name=(_name), .unit=(_unit), .min_value=(_min), \
      .mid_value=((_min)+(_max))*0.5f, .max_value=(_max), \
      .decimals=(_dec), .curve=NC_PARAM_LINEAR }

#define P_EXP(_name,_unit,_min,_max,_dec) \
    { .name=(_name), .unit=(_unit), .min_value=(_min), \
      .mid_value=0.0f, .max_value=(_max), \
      .decimals=(_dec), .curve=NC_PARAM_EXPONENTIAL }

#define P_ENUM(_name,_opts,_count) \
    { .name=(_name), .unit="", .min_value=0.0f, \
      .mid_value=0.5f, .max_value=(float)((_count)-1), \
      .decimals=0, .curve=NC_PARAM_LINEAR, \
      .options=(_opts), .option_count=(_count) }

static const char *const OPT_BOOL[] = {"OFF", "ON"};
static const char *const OPT_WAVE[] = {"Sine", "Triangle", "Square", "Saw"};

/* -------------------------------------------------------------------------
 * FX2
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t FX2_DRIVE_PARAMS[] = {
    P_LINEAR("Gain",  "",   0.0f, 1.0f, 2),
    P_LINEAR("Tone",  "",   0.0f, 100.0f, 0),
    P_LINEAR("Level", "",   0.0f, 100.0f, 0),
};
static const float FX2_DEF_DRIVE[] = {0.5f, 50.0f, 80.0f};

static const nanocore_param_info_t FX2_PITCH_PARAMS[] = {
    P_LINEAR("Pitch", "st", -24.0f, 24.0f, 0),
    P_LINEAR("Fine",  "",   -1.0f, 1.0f, 2),
    P_LINEAR("Mix",   "",    0.0f, 100.0f, 0),
};
static const float FX2_DEF_PITCH[] = {0.0f, 0.0f, 100.0f};

static const nanocore_param_info_t FX2_ENV_WAH_PARAMS[] = {
    P_EXP   ("Freq", "Hz", 10.0f, 20000.0f, 0),
    P_LINEAR("Env",  "Hz",  0.0f, 20000.0f, 0),
    P_LINEAR("Q",    "",    0.1f, 20.0f, 1),
    P_LINEAR("Mix",  "",    0.0f, 100.0f, 0),
};
static const float FX2_DEF_ENV_WAH[] = {400.0f, 1000.0f, 8.0f, 80.0f};

static const nanocore_param_info_t FX2_WAH_PARAMS[] = {
    P_EXP   ("Freq",  "Hz", 163.0f, 3500.0f, 0),
    P_LINEAR("Sweep", "",     0.0f, 100.0f, 0),
    P_LINEAR("Rate",  "Hz",   0.0f, 8.0f, 2),
    P_LINEAR("Shape", "",     0.0f, 100.0f, 0),
    P_LINEAR("Mix",   "",     0.0f, 100.0f, 0),
};
static const float FX2_DEF_WAH[] = {800.0f, 50.0f, 1.5f, 0.0f, 80.0f};

static const nanocore_param_info_t FX2_MOTION_WAH_PARAMS[] = {
    P_LINEAR("Rate",  "Hz", 0.0f, 15.0f, 1),
    P_LINEAR("Voice", "",   0.0f, 100.0f, 0),
    P_LINEAR("Sweep", "",   0.0f, 100.0f, 0),
    P_LINEAR("Mix",   "",   0.0f, 100.0f, 0),
};
static const float FX2_DEF_MOTION_WAH[] = {2.0f, 50.0f, 55.0f, 100.0f};

static const nanocore_effect_type_info_t FX2_BASE_TYPES[] = {
    {0,  "Scream",       3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {1,  "Klone",        3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {2,  "OCD",          3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {3,  "DS2",          3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {4,  "PIFUZZ",       3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {5,  "Range",        3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {6,  "XAC",          3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {7,  "Fiman",        3, FX2_DRIVE_PARAMS, NULL, FX2_DEF_DRIVE},
    {8,  "Pitch",        3, FX2_PITCH_PARAMS, NULL, FX2_DEF_PITCH},
    {9,  "Envelope Wah", 4, FX2_ENV_WAH_PARAMS, NULL, FX2_DEF_ENV_WAH},
    {10, "Wah",          5, FX2_WAH_PARAMS, NULL, FX2_DEF_WAH},
    {11, "Motion Wah",   4, FX2_MOTION_WAH_PARAMS, NULL, FX2_DEF_MOTION_WAH},
};

static const uint8_t FX2_FX1_MAP[9] = {0,1,2,3,7,8,9,10,11};

/* -------------------------------------------------------------------------
 * DELAY — firmware 1.2 includes Ping Pong in every type.
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t DEL_BBD[] = {
    P_LINEAR("Time",     "ms", 1.0f, 2000.0f, 0),
    P_LINEAR("Feedback", "",   0.0f, 95.0f, 0),
    P_LINEAR("Age",      "",   0.0f, 100.0f, 0),
    P_LINEAR("Mix",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_BBD[] = {350,48,65,40,0};

static const nanocore_param_info_t DEL_DIGITAL[] = {
    P_LINEAR("Time",     "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback", "",   0.0f, 95.0f, 0),
    P_LINEAR("Mix",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_DIGITAL[] = {350,32,32,0};

static const nanocore_param_info_t DEL_DUCK[] = {
    P_LINEAR("Time",        "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback",    "",   0.0f, 95.0f, 0),
    P_LINEAR("Sensitivity", "",   0.0f, 100.0f, 0),
    P_LINEAR("Depth",       "",   0.0f, 100.0f, 0),
    P_LINEAR("Attack",      "ms", 0.1f, 100.0f, 1),
    P_LINEAR("Release",     "ms", 10.0f, 2000.0f, 0),
    P_LINEAR("Filter",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
    P_LINEAR("Mix",         "",   0.0f, 100.0f, 0),
};
static const float DEF_DEL_DUCK[] = {450,42,50,50,5,200,55,0,40};

static const nanocore_param_info_t DEL_ICE[] = {
    P_LINEAR("Time",     "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback", "",   0.0f, 95.0f, 0),
    P_LINEAR("Interval", "st", -12.0f, 12.0f, 0),
    P_LINEAR("Mix",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_ICE[] = {501,30,12,40,0};

static const nanocore_param_info_t DEL_REVERSE[] = {
    P_LINEAR("Time",     "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback", "",   0.0f, 95.0f, 0),
    P_LINEAR("Mix",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_REVERSE[] = {600,25,50,0};

static const nanocore_param_info_t DEL_LOFI[] = {
    P_LINEAR("Time",      "ms", 2.0f, 3000.0f, 0),
    P_LINEAR("Feedback",  "",   0.0f, 95.0f, 0),
    P_LINEAR("Bit Depth", "bit",4.0f, 16.0f, 0),
    P_LINEAR("LoFi Mix",  "",   0.0f, 100.0f, 0),
    P_LINEAR("Mix",       "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_LOFI[] = {350,40,8,70,40,0};

static const nanocore_param_info_t DEL_SMEAR[] = {
    P_LINEAR("Time",     "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback", "",   0.0f, 95.0f, 0),
    P_LINEAR("Smear",    "",   0.0f, 100.0f, 0),
    P_LINEAR("Filter",   "",   0.0f, 100.0f, 0),
    P_LINEAR("Mix",      "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_SMEAR[] = {651,45,68,42,40,0};

static const nanocore_param_info_t DEL_MOD[] = {
    P_LINEAR("Time",      "ms", 0.1f, 3000.0f, 1),
    P_LINEAR("Feedback",  "",   0.0f, 95.0f, 0),
    P_LINEAR("Mod Depth", "ms", 0.0f, 20.0f, 1),
    P_LINEAR("Mod Rate",  "Hz", 0.05f, 5.0f, 2),
    P_LINEAR("Mix",       "",   0.0f, 100.0f, 0),
    P_ENUM  ("Ping Pong", OPT_BOOL, 2),
};
static const float DEF_DEL_MOD[] = {420,38,4,1,40,0};

static const nanocore_effect_type_info_t DELAY_TYPES[] = {
    {0,"BBD",5,DEL_BBD,NULL,DEF_DEL_BBD},
    {1,"Digital",4,DEL_DIGITAL,NULL,DEF_DEL_DIGITAL},
    {2,"Duck",9,DEL_DUCK,NULL,DEF_DEL_DUCK},
    {3,"Ice",5,DEL_ICE,NULL,DEF_DEL_ICE},
    {4,"Reverse",4,DEL_REVERSE,NULL,DEF_DEL_REVERSE},
    {5,"LoFi",6,DEL_LOFI,NULL,DEF_DEL_LOFI},
    {6,"Smear",6,DEL_SMEAR,NULL,DEF_DEL_SMEAR},
    {7,"Mod Delay",6,DEL_MOD,NULL,DEF_DEL_MOD},
};

/* -------------------------------------------------------------------------
 * MOD
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t MOD_CHORUS[] = {
    P_LINEAR("Rate","Hz",0.1f,3.74f,2),
    P_LINEAR("Depth","ms",0.0f,1.0f,2),
};
static const float DEF_MOD_CHORUS[] = {2.3f,0.5f};

static const nanocore_param_info_t MOD_PHASER[] = {
    P_LINEAR("Rate","Hz",0.05f,5.0f,2),
    P_LINEAR("Feedback","",-95.0f,95.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_PHASER[] = {0.5f,20,40};

static const nanocore_param_info_t MOD_FLANGER[] = {
    P_LINEAR("Rate","Hz",0.05f,5.0f,2),
    P_LINEAR("Feedback","",-95.0f,95.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_FLANGER[] = {0.8f,55,35};

static const nanocore_param_info_t MOD_TREMOLO[] = {
    P_LINEAR("Rate","Hz",0.01f,20.0f,2),
    P_LINEAR("Depth","",0.0f,100.0f,0),
    P_ENUM  ("Wave",OPT_WAVE,4),
    P_LINEAR("Smooth","Hz",0.0f,40.0f,2),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_TREMOLO[] = {5.01f,60,0,0,100};

static const nanocore_param_info_t MOD_VIBRATO[] = {
    P_LINEAR("Rate","Hz",0.01f,20.0f,2),
    P_LINEAR("Depth","ms",0.0f,20.0f,1),
    P_ENUM  ("Wave",OPT_WAVE,4),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_VIBRATO[] = {9.01f,0.4f,0,80};

static const nanocore_param_info_t MOD_VELVET[] = {
    P_EXP   ("Rate","Hz",0.45f,10.0f,2),
    P_LINEAR("Wave","",0.0f,100.0f,0),
    P_LINEAR("Voice","",0.0f,100.0f,0),
    P_LINEAR("Depth","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_VELVET[] = {1.0f,75,45,40,100};

static const nanocore_param_info_t MOD_CHORUS2[] = {
    P_LINEAR("Rate","",0.0f,1.0f,2),
    P_LINEAR("Amount","",0.0f,1.0f,2),
    P_LINEAR("Feedback","",0.0f,1.0f,2),
    P_LINEAR("Mix","",0.0f,1.0f,2),
};
static const float DEF_MOD_CHORUS2[] = {0.5f,0.5f,0.0f,0.5f};

static const nanocore_param_info_t MOD_PHASER2[] = {
    P_LINEAR("Depth","",0.0f,100.0f,0),
    P_LINEAR("Rate","Hz",0.05f,5.0f,2),
    P_LINEAR("Feedback","",-95.0f,95.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_PHASER2[] = {45,2.77f,0,65};

static const nanocore_param_info_t MOD_JET[] = {
    P_LINEAR("Rate","Hz",0.05f,5.0f,2),
    P_LINEAR("Depth","",0.0f,100.0f,0),
    P_LINEAR("Feedback","",-95.0f,95.0f,0),
    P_LINEAR("Phase","deg",0.0f,360.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_MOD_JET[] = {0.55f,70,5,95,55};

static const nanocore_effect_type_info_t MOD_TYPES[] = {
    {0,"Chorus",2,MOD_CHORUS,NULL,DEF_MOD_CHORUS},
    {1,"Phaser",3,MOD_PHASER,NULL,DEF_MOD_PHASER},
    {2,"Flanger",3,MOD_FLANGER,NULL,DEF_MOD_FLANGER},
    {3,"Tremolo",5,MOD_TREMOLO,NULL,DEF_MOD_TREMOLO},
    {4,"Vibrato",4,MOD_VIBRATO,NULL,DEF_MOD_VIBRATO},
    {10,"Velvet Vibrato",5,MOD_VELVET,NULL,DEF_MOD_VELVET},
    {11,"Chorus II",4,MOD_CHORUS2,NULL,DEF_MOD_CHORUS2},
    {12,"Phaser II",4,MOD_PHASER2,NULL,DEF_MOD_PHASER2},
    {17,"Jet Flanger",5,MOD_JET,NULL,DEF_MOD_JET},
};
static const uint8_t MOD_ORDER[] = {0,11,1,12,2,17,4,10,3};

/* -------------------------------------------------------------------------
 * REVERB
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t REV_ROOM[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("Mod","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_ROOM[] = {1.2f,5,65,10,30};

static const nanocore_param_info_t REV_PLATE[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("Mod","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_PLATE[] = {2.5f,12,70,25,35};

static const nanocore_param_info_t REV_HALL[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("LF Damp","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_HALL[] = {4,24,58,40,38};

static const nanocore_param_info_t REV_CONCERT[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0),
    P_LINEAR("HF Damp","",0.0f,100.0f,0),
    P_LINEAR("Mod","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_CONCERT[] = {5.5f,32,55,30,40};

static const nanocore_param_info_t REV_SPRING[] = {
    P_LINEAR("Decay","s",0.0f,12.0f,1),
    P_LINEAR("Pre Delay","ms",5.0f,80.0f,0),
    P_LINEAR("Crossover","Hz",800.0f,8000.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_SPRING[] = {2,20,4501,20,50};

static const nanocore_param_info_t REV_SHIMMER[] = {
    P_LINEAR("Decay","s",0.0f,12.0f,1),
    P_LINEAR("Pre Delay","ms",5.0f,120.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("Pitch","st",-24.0f,24.0f,0),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_SHIMMER[] = {3.2f,5,58,12,50};

static const nanocore_param_info_t REV_CLOUD[] = {
    P_LINEAR("Decay","s",1.0f,100.0f,0),
    P_LINEAR("Tone","",0.0f,100.0f,0),
    P_LINEAR("Mod Depth","",0.0f,100.0f,0),
    P_LINEAR("Mod Rate","Hz",0.02f,0.25f,2),
    P_LINEAR("Mix","",0.0f,100.0f,0),
};
static const float DEF_REV_CLOUD[] = {55,75,40,0.1f,50};

static const nanocore_effect_type_info_t REV_TYPES[] = {
    {0,"Room",5,REV_ROOM,NULL,DEF_REV_ROOM},
    {1,"Plate",5,REV_PLATE,NULL,DEF_REV_PLATE},
    {2,"Hall",5,REV_HALL,NULL,DEF_REV_HALL},
    {3,"Concert",5,REV_CONCERT,NULL,DEF_REV_CONCERT},
    {5,"Cloud",5,REV_CLOUD,NULL,DEF_REV_CLOUD},
    {6,"Spring",5,REV_SPRING,NULL,DEF_REV_SPRING},
    {7,"Shimmer",5,REV_SHIMMER,NULL,DEF_REV_SHIMMER},
};
static const uint8_t REV_ORDER[] = {0,1,2,3,6,7,5};

/* -------------------------------------------------------------------------
 * EQ
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t EQ3[] = {
    P_LINEAR("Low","dB",-15.0f,15.0f,0),
    P_LINEAR("Mid","dB",-15.0f,15.0f,0),
    P_LINEAR("High","dB",-15.0f,15.0f,0),
    P_LINEAR("Level","dB",-12.0f,12.0f,0),
};
static const float DEF_EQ3[] = {0,0,0,0};

static const nanocore_param_info_t EQ6[] = {
    P_LINEAR("120 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("250 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("500 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("1 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("2 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("4 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("Level","dB",-12.0f,12.0f,0),
};
static const float DEF_EQ6[] = {0,0,0,0,0,0,0};

static const nanocore_param_info_t EQ8[] = {
    P_LINEAR("60 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("120 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("250 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("500 Hz","dB",-15.0f,15.0f,0),
    P_LINEAR("1 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("2 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("4 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("8 kHz","dB",-15.0f,15.0f,0),
    P_LINEAR("Level","dB",-12.0f,12.0f,0),
};
static const float DEF_EQ8[] = {0,0,0,0,0,0,0,0,0};

static const nanocore_effect_type_info_t EQ_TYPES[] = {
    {0,"3-band EQ",4,EQ3,NULL,DEF_EQ3},
    {1,"6-band EQ",7,EQ6,NULL,DEF_EQ6},
    {2,"8-band EQ",9,EQ8,NULL,DEF_EQ8},
};

/* -------------------------------------------------------------------------
 * AMP/CAB common live parameters
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t AMP_COMMON[] = {
    P_LINEAR("Gain","",0.0f,1.0f,2),
    P_LINEAR("Bass","dB",-10.0f,10.0f,0),
    P_LINEAR("Mid","dB",-10.0f,10.0f,0),
    P_LINEAR("Treble","dB",-10.0f,10.0f,0),
    P_LINEAR("Level","",0.0f,100.0f,0),
};

static const nanocore_param_info_t CAB_COMMON[] = {
    P_EXP   ("Low Cut","Hz",20.0f,500.0f,0),
    P_EXP   ("High Cut","Hz",2000.0f,20000.0f,0),
    P_LINEAR("Level","",0.0f,100.0f,0),
};

static const nanocore_effect_type_info_t *lookup(
    const nanocore_effect_type_info_t *items,
    size_t count,
    uint8_t raw)
{
    for (size_t i=0;i<count;++i)
        if (items[i].id == raw) return &items[i];
    return NULL;
}

const nanocore_effect_type_info_t *nanocore_spec_block_type(
    controller_block_t block, uint8_t raw_type_id)
{
    if (block == BLOCK_FX1)
        return nanocore_spec_fx1_type(raw_type_id);

    if (block == BLOCK_FX2) {
        if (raw_type_id <= 11)
            return lookup(FX2_BASE_TYPES,
                          sizeof(FX2_BASE_TYPES)/sizeof(FX2_BASE_TYPES[0]),
                          raw_type_id);

        if (raw_type_id >= 12 && raw_type_id <= 20) {
            const nanocore_effect_type_info_t *src =
                nanocore_spec_fx1_type(FX2_FX1_MAP[raw_type_id-12]);
            static nanocore_effect_type_info_t converted;
            if (!src) return NULL;
            converted = *src;
            converted.id = raw_type_id;
            return &converted;
        }
        return NULL;
    }

    switch (block) {
    case BLOCK_DELAY:
        return lookup(DELAY_TYPES,sizeof(DELAY_TYPES)/sizeof(DELAY_TYPES[0]),
                      raw_type_id);
    case BLOCK_MOD:
        return lookup(MOD_TYPES,sizeof(MOD_TYPES)/sizeof(MOD_TYPES[0]),
                      raw_type_id);
    case BLOCK_REVERB:
        return lookup(REV_TYPES,sizeof(REV_TYPES)/sizeof(REV_TYPES[0]),
                      raw_type_id);
    case BLOCK_EQ:
        return lookup(EQ_TYPES,sizeof(EQ_TYPES)/sizeof(EQ_TYPES[0]),
                      raw_type_id);
    default:
        return NULL;
    }
}

size_t nanocore_spec_block_selector_count(controller_block_t block)
{
    switch (block) {
    case BLOCK_FX1: return nanocore_spec_fx1_selector_count();
    case BLOCK_FX2: return 21;
    case BLOCK_AMP: return nanocore_spec_amp_model_count();
    case BLOCK_CAB: return nanocore_spec_cab_model_count();
    case BLOCK_DELAY: return 8;
    case BLOCK_MOD: return sizeof(MOD_ORDER)/sizeof(MOD_ORDER[0]);
    case BLOCK_REVERB: return sizeof(REV_ORDER)/sizeof(REV_ORDER[0]);
    case BLOCK_EQ: return 3;
    default: return 0;
    }
}

const nanocore_effect_type_info_t *nanocore_spec_block_selector(
    controller_block_t block, size_t index)
{
    if (block == BLOCK_FX1)
        return nanocore_spec_fx1_selector(index);

    if (block == BLOCK_FX2) {
        if (index >= 21) return NULL;
        return nanocore_spec_block_type(block,(uint8_t)index);
    }

    if (block == BLOCK_DELAY) {
        if (index >= 8) return NULL;
        return &DELAY_TYPES[index];
    }

    if (block == BLOCK_MOD) {
        if (index >= sizeof(MOD_ORDER)/sizeof(MOD_ORDER[0])) return NULL;
        return nanocore_spec_block_type(block,MOD_ORDER[index]);
    }

    if (block == BLOCK_REVERB) {
        if (index >= sizeof(REV_ORDER)/sizeof(REV_ORDER[0])) return NULL;
        return nanocore_spec_block_type(block,REV_ORDER[index]);
    }

    if (block == BLOCK_EQ) {
        if (index >= 3) return NULL;
        return &EQ_TYPES[index];
    }

    return NULL;
}

int nanocore_spec_block_selector_index(
    controller_block_t block, uint8_t raw_type_id)
{
    size_t count = nanocore_spec_block_selector_count(block);
    for (size_t i=0;i<count;++i) {
        if (block == BLOCK_AMP || block == BLOCK_CAB)
            return raw_type_id < count ? (int)raw_type_id : -1;

        const nanocore_effect_type_info_t *info =
            nanocore_spec_block_selector(block,i);
        if (info && info->id == raw_type_id) return (int)i;
    }
    return -1;
}

const nanocore_param_info_t *nanocore_spec_common_params(
    controller_block_t block, uint8_t *count)
{
    if (count) *count = 0;
    if (block == BLOCK_AMP) {
        if (count) *count = sizeof(AMP_COMMON)/sizeof(AMP_COMMON[0]);
        return AMP_COMMON;
    }
    if (block == BLOCK_CAB) {
        if (count) *count = sizeof(CAB_COMMON)/sizeof(CAB_COMMON[0]);
        return CAB_COMMON;
    }
    return NULL;
}

bool nanocore_spec_fill_type_defaults(
    controller_block_t block,
    uint8_t raw_type_id,
    float *out_normalized,
    uint8_t out_capacity)
{
    const nanocore_effect_type_info_t *info =
        nanocore_spec_block_type(block,raw_type_id);
    if (!info || !out_normalized ||
        out_capacity < info->expected_param_count) return false;

    if (info->default_normalized) {
        memcpy(out_normalized,info->default_normalized,
               (size_t)info->expected_param_count*sizeof(float));
        return true;
    }

    if (!info->default_real || !info->params) return false;
    for (uint8_t i=0;i<info->expected_param_count;++i) {
        out_normalized[i] =
            nanocore_spec_real_to_normalized(
                &info->params[i],info->default_real[i]);
    }
    return true;
}

const char *nanocore_spec_discrete_label(
    const nanocore_param_info_t *param,
    float normalized)
{
    if (!param || !param->options || param->option_count == 0)
        return NULL;

    int idx = (int)lroundf(normalized *
                           (float)(param->option_count - 1));
    if (idx < 0) idx = 0;
    if (idx >= param->option_count) idx = param->option_count - 1;
    return param->options[idx];
}

float nanocore_spec_adjust_normalized(
    const nanocore_param_info_t *param,
    float current,
    int delta)
{
    if (current < 0.0f) current = 0.0f;
    if (current > 1.0f) current = 1.0f;

    if (param && param->options && param->option_count > 1) {
        int idx = (int)lroundf(current *
                              (float)(param->option_count - 1));
        idx += delta;
        if (idx < 0) idx = 0;
        if (idx >= param->option_count) idx = param->option_count - 1;
        return (float)idx / (float)(param->option_count - 1);
    }

    float next = current + (float)delta * 0.01f;
    if (next < 0.0f) next = 0.0f;
    if (next > 1.0f) next = 1.0f;
    return next;
}
