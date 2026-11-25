#include "nanocore_spec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define P_LINEAR(_name,_unit,_min,_max,_dec,_def) \
    { .name=(_name), .unit=(_unit), \
      .min_value=(_min), .mid_value=((_min)+(_max))*0.5f, \
      .max_value=(_max), .decimals=(_dec), .curve=NC_PARAM_LINEAR, \
      .default_value=(_def), .value_kind=NC_PARAM_VALUE_NUMERIC }

#define P_EXP(_name,_unit,_min,_max,_dec,_def) \
    { .name=(_name), .unit=(_unit), \
      .min_value=(_min), .mid_value=0.0f, .max_value=(_max), \
      .decimals=(_dec), .curve=NC_PARAM_EXPONENTIAL, \
      .default_value=(_def), .value_kind=NC_PARAM_VALUE_NUMERIC }

#define P_CUBIC(_name,_unit,_min,_max,_dec,_def) \
    { .name=(_name), .unit=(_unit), \
      .min_value=(_min), .mid_value=0.0f, .max_value=(_max), \
      .decimals=(_dec), .curve=NC_PARAM_CUBIC, \
      .default_value=(_def), .value_kind=NC_PARAM_VALUE_NUMERIC }

#define P_BOOL(_name,_def) \
    { .name=(_name), .unit="", \
      .min_value=0.0f, .mid_value=0.5f, .max_value=1.0f, \
      .decimals=0, .curve=NC_PARAM_LINEAR, \
      .default_value=(_def), .value_kind=NC_PARAM_VALUE_BOOLEAN }

#define P_ENUM(_name,_opts,_count,_def) \
    { .name=(_name), .unit="", \
      .min_value=0.0f, .mid_value=((float)((_count)-1))*0.5f, \
      .max_value=(float)((_count)-1), .decimals=0, \
      .curve=NC_PARAM_LINEAR, .default_value=(float)(_def), \
      .value_kind=NC_PARAM_VALUE_ENUM, \
      .options=(_opts), .option_count=(_count) }

#define TYPE(_id,_name,_params,_defs) \
    { (_id), (_name), \
      (uint8_t)(sizeof(_params)/sizeof((_params)[0])), (_params), (_defs) }

/* -------------------------------------------------------------------------
 * FX2
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t FX2_DRIVE_PARAMS[] = {
    P_LINEAR("Gain",  "", 0.0f, 1.0f,   2, 0.5f),
    P_LINEAR("Tone",  "", 0.0f, 100.0f, 0, 50.0f),
    P_LINEAR("Level", "", 0.0f, 100.0f, 0, 80.0f),
};

static const nanocore_param_info_t FX2_PITCH_PARAMS[] = {
    P_LINEAR("Pitch", "st", -24.0f, 24.0f, 0, 0.0f),
    P_LINEAR("Fine",  "",    -1.0f,  1.0f, 2, 0.0f),
    P_LINEAR("Mix",   "",     0.0f, 100.0f, 0, 100.0f),
};

static const nanocore_effect_type_info_t FX2_OWN_TYPES[] = {
    TYPE(0, "Scream", FX2_DRIVE_PARAMS, NULL),
    TYPE(1, "Klone",  FX2_DRIVE_PARAMS, NULL),
    TYPE(2, "OCD",    FX2_DRIVE_PARAMS, NULL),
    TYPE(3, "DS2",    FX2_DRIVE_PARAMS, NULL),
    TYPE(4, "PIFUZZ", FX2_DRIVE_PARAMS, NULL),
    TYPE(5, "Range",  FX2_DRIVE_PARAMS, NULL),
    TYPE(6, "XAC",    FX2_DRIVE_PARAMS, NULL),
    TYPE(7, "Fiman",  FX2_DRIVE_PARAMS, NULL),
    TYPE(8, "Pitch",  FX2_PITCH_PARAMS, NULL),
};

typedef struct {
    uint8_t raw;
    uint8_t fx1_raw;
    const char *name;
} fx2_alias_map_t;

static const fx2_alias_map_t FX2_ALIAS_MAP[] = {
    { 9,  4, "Envelope Wah" },
    { 10, 5, "Wah" },
    { 11, 6, "Motion Wah" },
    { 12, 0, "Gate" },
    { 13, 1, "Auto Gate" },
    { 14, 2, "Gate + Compressor" },
    { 15, 3, "Compressor" },
    { 16, 7, "Peak Comp" },
    { 17, 8, "Parallel Comp" },
    { 18, 9, "Classic Wah" },
    { 19,10, "Retro Wah" },
    { 20,11, "Bass Wah" },
};

static nanocore_effect_type_info_t s_fx2_alias_types[
    sizeof(FX2_ALIAS_MAP)/sizeof(FX2_ALIAS_MAP[0])];
static bool s_fx2_alias_ready;

static void init_fx2_aliases(void)
{
    if (s_fx2_alias_ready) return;

    for (size_t i=0;
         i<sizeof(FX2_ALIAS_MAP)/sizeof(FX2_ALIAS_MAP[0]);
         ++i) {
        const nanocore_effect_type_info_t *src =
            nanocore_spec_fx1_type(FX2_ALIAS_MAP[i].fx1_raw);

        if (!src) continue;

        s_fx2_alias_types[i] = *src;
        s_fx2_alias_types[i].id = FX2_ALIAS_MAP[i].raw;
        s_fx2_alias_types[i].name = FX2_ALIAS_MAP[i].name;
    }

    s_fx2_alias_ready = true;
}

/* -------------------------------------------------------------------------
 * DELAY — firmware 1.2, including Ping Pong.
 * Physical Time range is 0..2000-ish ms on the user's unit; the normalized
 * captures line up with a 2000 ms ceiling, so Stage 07 follows hardware.
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t DEL_BBD[] = {
    P_LINEAR("Time",      "ms", 1.0f, 2000.0f, 1, 350.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 48.0f),
    P_LINEAR("Age",       "",   0.0f,  100.0f, 0, 65.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 40.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_DIGITAL[] = {
    P_LINEAR("Time",      "ms", 0.1f, 2000.0f, 1, 350.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 32.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 32.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_DUCK[] = {
    P_LINEAR("Time",        "ms", 0.1f, 2000.0f, 1, 450.0f),
    P_LINEAR("Feedback",    "",   0.0f,   95.0f, 0, 42.0f),
    P_LINEAR("Sensitivity", "",   0.0f,  100.0f, 0, 55.0f),
    P_LINEAR("Depth",       "",   0.0f,  100.0f, 0, 100.0f),
    P_LINEAR("Attack",      "ms", 0.1f,  100.0f, 1, 5.0f),
    P_LINEAR("Release",     "ms",10.0f, 2000.0f, 0, 350.0f),
    P_LINEAR("Filter",      "",   0.0f,  100.0f, 0, 55.0f),
    P_BOOL  ("Ping Pong", 0.0f),
    P_LINEAR("Mix",         "",   0.0f,  100.0f, 0, 40.0f),
};

static const nanocore_param_info_t DEL_ICE[] = {
    P_LINEAR("Time",      "ms", 0.1f, 2000.0f, 1, 500.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 30.0f),
    P_LINEAR("Interval",  "st",-12.0f,   12.0f, 0, 12.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 42.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_REVERSE[] = {
    P_LINEAR("Time",      "ms", 0.1f, 2000.0f, 1, 600.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 25.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 50.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_LOFI[] = {
    P_LINEAR("Time",      "ms", 2.0f, 2000.0f, 1, 360.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 40.0f),
    P_LINEAR("Bit Depth", "bit",4.0f,   16.0f, 0, 8.0f),
    P_LINEAR("LoFi Mix",  "",   0.0f,  100.0f, 0, 70.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 38.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_SMEAR[] = {
    P_LINEAR("Time",      "ms", 0.1f, 2000.0f, 1, 650.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 45.0f),
    P_LINEAR("Smear",     "",   0.0f,  100.0f, 0, 68.0f),
    P_LINEAR("Filter",    "",   0.0f,  100.0f, 0, 42.0f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 42.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const nanocore_param_info_t DEL_MOD[] = {
    P_LINEAR("Time",      "ms", 0.1f, 2000.0f, 1, 420.0f),
    P_LINEAR("Feedback",  "",   0.0f,   95.0f, 0, 38.0f),
    P_LINEAR("Mod Depth", "ms", 0.0f,   20.0f, 1, 4.4f),
    P_LINEAR("Mod Rate",  "Hz", 0.05f,   5.0f, 2, 0.5f),
    P_LINEAR("Mix",       "",   0.0f,  100.0f, 0, 38.0f),
    P_BOOL  ("Ping Pong", 0.0f),
};

static const float DEL_DEF_DUCK[] = {
    0.224961f,0.442105f,0.55f,1.0f,0.049049f,
    0.170854f,0.55f,0.0f,0.40f
};
static const float DEL_DEF_ICE[] =
    {0.249962f,0.315789f,1.0f,0.42f,0.0f};
static const float DEL_DEF_REVERSE[] =
    {0.299965f,0.263158f,0.50f,0.0f};
static const float DEL_DEF_LOFI[] =
    {0.179179f,0.421053f,0.333333f,0.70f,0.38f,0.0f};
static const float DEL_DEF_SMEAR[] =
    {0.324966f,0.473684f,0.68f,0.42f,0.42f,0.0f};
static const float DEL_DEF_MOD[] =
    {0.209960f,0.40f,0.22f,0.090909f,0.38f,0.0f};

static const nanocore_effect_type_info_t DELAY_TYPES[] = {
    TYPE(0, "BBD",       DEL_BBD,     NULL),
    TYPE(1, "Digital",   DEL_DIGITAL, NULL),
    TYPE(2, "Duck",      DEL_DUCK,    DEL_DEF_DUCK),
    TYPE(3, "Ice",       DEL_ICE,     DEL_DEF_ICE),
    TYPE(4, "Reverse",   DEL_REVERSE, DEL_DEF_REVERSE),
    TYPE(5, "LoFi",      DEL_LOFI,    DEL_DEF_LOFI),
    TYPE(6, "Smear",     DEL_SMEAR,   DEL_DEF_SMEAR),
    TYPE(7, "Mod Delay", DEL_MOD,     DEL_DEF_MOD),
};

/* -------------------------------------------------------------------------
 * MOD
 * ------------------------------------------------------------------------- */

static const char * const WAVE_OPTIONS[] = {
    "Sine", "Triangle", "Square", "Saw"
};

static const nanocore_param_info_t MOD_CHORUS[] = {
    P_LINEAR("Rate",  "Hz", 0.10f, 3.74f, 2, 2.30f),
    P_LINEAR("Depth", "ms", 0.0f,  1.0f,  2, 0.50f),
};
static const nanocore_param_info_t MOD_PHASER[] = {
    P_LINEAR("Rate",     "Hz", 0.05f, 5.0f, 2, 0.50f),
    P_LINEAR("Feedback", "", -95.0f, 95.0f, 0, 20.0f),
    P_LINEAR("Mix",      "",   0.0f,100.0f, 0, 40.0f),
};
static const nanocore_param_info_t MOD_FLANGER[] = {
    P_LINEAR("Rate",     "Hz", 0.05f, 5.0f, 2, 0.80f),
    P_LINEAR("Feedback", "", -95.0f, 95.0f, 0, 55.0f),
    P_LINEAR("Mix",      "",   0.0f,100.0f, 0, 35.0f),
};
static const nanocore_param_info_t MOD_TREMOLO[] = {
    P_LINEAR("Rate",   "Hz", 0.01f,20.0f,2,5.01f),
    P_LINEAR("Depth",  "",    0.0f,100.0f,0,60.0f),
    P_ENUM  ("Wave", WAVE_OPTIONS, 4, 0),
    P_LINEAR("Smooth", "Hz",  0.0f,40.0f,2,0.0f),
    P_LINEAR("Mix",    "",    0.0f,100.0f,0,100.0f),
};
static const nanocore_param_info_t MOD_VIBRATO[] = {
    P_LINEAR("Rate",  "Hz", 0.01f,20.0f,2,9.01f),
    P_LINEAR("Depth", "ms", 0.0f,20.0f,1,0.40f),
    P_ENUM  ("Wave", WAVE_OPTIONS, 4, 0),
    P_LINEAR("Mix",   "",   0.0f,100.0f,0,80.0f),
};
static const nanocore_param_info_t MOD_VELVET[] = {
    P_EXP   ("Rate",  "Hz", 0.45f,10.0f,2,1.0f),
    P_LINEAR("Wave",  "",    0.0f,100.0f,0,75.0f),
    P_LINEAR("Voice", "",    0.0f,100.0f,0,45.0f),
    P_LINEAR("Depth", "",    0.0f,100.0f,0,40.0f),
    P_LINEAR("Mix",   "",    0.0f,100.0f,0,100.0f),
};
static const nanocore_param_info_t MOD_CHORUS2[] = {
    P_LINEAR("Rate",     "",0.0f,1.0f,2,0.50f),
    P_LINEAR("Amount",   "",0.0f,1.0f,2,0.50f),
    P_LINEAR("Feedback", "",0.0f,1.0f,2,0.0f),
    P_LINEAR("Mix",      "",0.0f,1.0f,2,0.50f),
};
static const nanocore_param_info_t MOD_PHASER2[] = {
    P_LINEAR("Depth",    "",   0.0f,100.0f,0,45.0f),
    P_LINEAR("Rate",     "Hz", 0.05f,5.0f,2,2.77f),
    P_LINEAR("Feedback", "", -95.0f,95.0f,0,0.0f),
    P_LINEAR("Mix",      "",   0.0f,100.0f,0,65.0f),
};
static const nanocore_param_info_t MOD_JET[] = {
    P_LINEAR("Rate",     "Hz",0.05f,5.0f,2,0.55f),
    P_LINEAR("Depth",    "",  0.0f,100.0f,0,70.0f),
    P_LINEAR("Feedback", "",-95.0f,95.0f,0,5.0f),
    P_LINEAR("Phase",    "deg",0.0f,360.0f,0,95.0f),
    P_LINEAR("Mix",      "",0.0f,100.0f,0,55.0f),
};

static const float MOD_DEF_CHORUS[]  = {0.53f,0.60f};
static const float MOD_DEF_PHASER[]  = {0.151515f,0.605263f,0.40f};
static const float MOD_DEF_FLANGER[] = {0.151515f,0.789474f,0.35f};
static const float MOD_DEF_TREM[]    = {0.249625f,0.60f,0.0f,0.0f,1.0f};
static const float MOD_DEF_VIB[]     = {0.449725f,0.02f,0.0f,0.80f};
static const float MOD_DEF_VELVET[]  = {0.257492f,0.75f,0.45f,0.40f,1.0f};
static const float MOD_DEF_CHORUS2[] = {0.15f,0.45f,0.30f,0.50f};
static const float MOD_DEF_PHASER2[] = {0.45f,0.55f,0.50f,0.65f};
static const float MOD_DEF_JET[]     =
    {0.100503f,0.70f,0.526316f,0.263889f,0.55f};

static const nanocore_effect_type_info_t MOD_TYPES[] = {
    TYPE(0,  "Chorus",         MOD_CHORUS,  MOD_DEF_CHORUS),
    TYPE(1,  "Phaser",         MOD_PHASER,  MOD_DEF_PHASER),
    TYPE(2,  "Flanger",        MOD_FLANGER, MOD_DEF_FLANGER),
    TYPE(3,  "Tremolo",        MOD_TREMOLO, MOD_DEF_TREM),
    TYPE(4,  "Vibrato",        MOD_VIBRATO, MOD_DEF_VIB),
    TYPE(10, "Velvet Vibrato", MOD_VELVET,  MOD_DEF_VELVET),
    TYPE(11, "Chorus II",      MOD_CHORUS2, MOD_DEF_CHORUS2),
    TYPE(12, "Phaser II",      MOD_PHASER2, MOD_DEF_PHASER2),
    TYPE(17, "Jet Flanger",    MOD_JET,     MOD_DEF_JET),
};

static const uint8_t MOD_SELECTOR_ORDER[] = {
    0, 11, 1, 12, 2, 17, 4, 10, 3
};

/* -------------------------------------------------------------------------
 * REVERB
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t REV_ROOM[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1,1.2f),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0,5.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,65.0f),
    P_LINEAR("Mod","",0.0f,100.0f,0,10.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,30.0f),
};
static const nanocore_param_info_t REV_PLATE[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1,2.5f),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0,12.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,70.0f),
    P_LINEAR("Mod","",0.0f,100.0f,0,25.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,35.0f),
};
static const nanocore_param_info_t REV_HALL[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1,4.0f),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0,24.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,58.0f),
    P_LINEAR("LF Damp","",0.0f,100.0f,0,40.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,38.0f),
};
static const nanocore_param_info_t REV_CONCERT[] = {
    P_LINEAR("Decay","s",0.5f,12.0f,1,5.5f),
    P_LINEAR("Pre Delay","ms",0.0f,80.0f,0,32.0f),
    P_LINEAR("HF Damp","",0.0f,100.0f,0,55.0f),
    P_LINEAR("Mod","",0.0f,100.0f,0,30.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,40.0f),
};
static const nanocore_param_info_t REV_SPRING[] = {
    P_LINEAR("Decay","s",0.0f,12.0f,1,2.0f),
    P_LINEAR("Pre Delay","ms",5.0f,80.0f,0,20.0f),
    P_LINEAR("Crossover","Hz",800.0f,8000.0f,0,4500.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,20.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,50.0f),
};
static const nanocore_param_info_t REV_SHIMMER[] = {
    P_LINEAR("Decay","s",0.0f,12.0f,1,6.0f),
    P_LINEAR("Pre Delay","ms",5.0f,120.0f,0,28.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,50.0f),
    P_LINEAR("Pitch","st",-24.0f,24.0f,0,0.0f),
    P_LINEAR("Mix","",0.0f,100.0f,0,50.0f),
};
static const nanocore_param_info_t REV_CLOUD[] = {
    P_LINEAR("Decay","s",1.0f,100.0f,0,50.0f),
    P_LINEAR("Tone","",0.0f,100.0f,0,50.0f),
    P_LINEAR("Mod Depth","",0.0f,100.0f,0,50.0f),
    P_LINEAR("Mod Rate","Hz",0.02f,0.25f,2,0.25f),
    P_LINEAR("Mix","",0.0f,100.0f,0,50.0f),
};

static const float REV_DEF_ROOM[] =
    {0.060870f,0.0625f,0.65f,0.10f,0.30f};
static const float REV_DEF_PLATE[] =
    {0.165217f,0.15f,0.70f,0.25f,0.35f};
static const float REV_DEF_HALL[] =
    {0.304348f,0.30f,0.58f,0.40f,0.38f};
static const float REV_DEF_CONCERT[] =
    {0.434783f,0.40f,0.55f,0.30f,0.40f};
static const float REV_DEF_SPRING[] =
    {0.55f,0.75f,0.40f,0.26f,0.50f};
static const float REV_DEF_SHIMMER[] =
    {0.50f,0.20f,0.50f,0.50f,0.50f};
static const float REV_DEF_CLOUD[] =
    {0.50f,0.50f,0.50f,1.0f,0.50f};

static const nanocore_effect_type_info_t REVERB_TYPES[] = {
    TYPE(0, "Room",    REV_ROOM,    REV_DEF_ROOM),
    TYPE(1, "Plate",   REV_PLATE,   REV_DEF_PLATE),
    TYPE(2, "Hall",    REV_HALL,    REV_DEF_HALL),
    TYPE(3, "Concert", REV_CONCERT, REV_DEF_CONCERT),
    TYPE(6, "Spring",  REV_SPRING,  REV_DEF_SPRING),
    TYPE(7, "Shimmer", REV_SHIMMER, REV_DEF_SHIMMER),
    TYPE(5, "Cloud",   REV_CLOUD,   REV_DEF_CLOUD),
};

static const uint8_t REVERB_SELECTOR_ORDER[] = {
    0,1,2,3,6,7,5
};

/* -------------------------------------------------------------------------
 * EQ
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t EQ3[] = {
    P_LINEAR("Low",  "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("Mid",  "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("High", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("Level","dB",-12.0f,12.0f,1,0.0f),
};
static const nanocore_param_info_t EQ6[] = {
    P_LINEAR("120 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("250 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("500 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("1 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("2 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("4 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("Level", "dB",-12.0f,12.0f,1,0.0f),
};
static const nanocore_param_info_t EQ8[] = {
    P_LINEAR("60 Hz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("120 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("250 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("500 Hz","dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("1 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("2 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("4 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("8 kHz", "dB",-15.0f,15.0f,1,0.0f),
    P_LINEAR("Level", "dB",-12.0f,12.0f,1,0.0f),
};
static const float EQ3_DEF[] = {0.5f,0.5f,0.5f,0.5f};
static const float EQ6_DEF[] =
    {0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f};
static const float EQ8_DEF[] =
    {0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f};

static const nanocore_effect_type_info_t EQ_TYPES[] = {
    TYPE(0,"3-band EQ",EQ3,EQ3_DEF),
    TYPE(1,"6-band EQ",EQ6,EQ6_DEF),
    TYPE(2,"8-band EQ",EQ8,EQ8_DEF),
};

/* -------------------------------------------------------------------------
 * AMP / CAB common profiles and model catalogs
 * ------------------------------------------------------------------------- */

static const nanocore_param_info_t AMP_COMMON[] = {
    P_LINEAR("Gain",   "",    0.0f,  1.0f,2,1.0f),
    P_LINEAR("Bass",   "dB",-10.0f, 10.0f,0,0.0f),
    P_LINEAR("Mid",    "dB",-10.0f, 10.0f,0,0.0f),
    P_LINEAR("Treble", "dB",-10.0f, 10.0f,0,0.0f),
    P_LINEAR("Level",  "",    0.0f,100.0f,0,80.0f),
};
static const nanocore_param_info_t CAB_COMMON[] = {
    P_EXP   ("Low Cut", "Hz",20.0f,500.0f,1,20.0f),
    P_EXP   ("High Cut","Hz",2000.0f,20000.0f,0,20000.0f),
    P_LINEAR("Level",   "",0.0f,100.0f,0,100.0f),
};

static nanocore_effect_type_info_t s_amp_types[30];
static nanocore_effect_type_info_t s_cab_types[30];
static bool s_model_types_ready;

static void init_model_types(void)
{
    if (s_model_types_ready) return;

    for (uint8_t i=0;i<30;++i) {
        s_amp_types[i].id=i;
        s_amp_types[i].name=nanocore_spec_amp_model_name(i);
        s_amp_types[i].expected_param_count=
            (uint8_t)(sizeof(AMP_COMMON)/sizeof(AMP_COMMON[0]));
        s_amp_types[i].params=AMP_COMMON;
        s_amp_types[i].default_normalized=NULL;

        s_cab_types[i].id=i;
        s_cab_types[i].name=nanocore_spec_cab_model_name(i);
        s_cab_types[i].expected_param_count=
            (uint8_t)(sizeof(CAB_COMMON)/sizeof(CAB_COMMON[0]));
        s_cab_types[i].params=CAB_COMMON;
        s_cab_types[i].default_normalized=NULL;
    }

    s_model_types_ready=true;
}

/* -------------------------------------------------------------------------
 * Generic lookup
 * ------------------------------------------------------------------------- */

static const nanocore_effect_type_info_t *find_type(
    const nanocore_effect_type_info_t *items,
    size_t count,
    uint8_t raw)
{
    for (size_t i=0;i<count;++i) {
        if (items[i].id==raw) return &items[i];
    }
    return NULL;
}

static const nanocore_effect_type_info_t *fx2_type(uint8_t raw)
{
    if (raw <= 8) {
        return find_type(FX2_OWN_TYPES,
                         sizeof(FX2_OWN_TYPES)/sizeof(FX2_OWN_TYPES[0]),
                         raw);
    }

    init_fx2_aliases();
    return find_type(s_fx2_alias_types,
                     sizeof(s_fx2_alias_types)/sizeof(s_fx2_alias_types[0]),
                     raw);
}

bool nanocore_spec_editor_is_model_block(controller_block_t block)
{
    return block==BLOCK_AMP || block==BLOCK_CAB;
}

const nanocore_effect_type_info_t *nanocore_spec_editor_type_by_raw(
    controller_block_t block,
    uint8_t raw)
{
    switch (block) {
    case BLOCK_FX1:
        return nanocore_spec_fx1_type(raw);
    case BLOCK_FX2:
        return fx2_type(raw);
    case BLOCK_AMP:
        init_model_types();
        return raw<30 ? &s_amp_types[raw] : NULL;
    case BLOCK_CAB:
        init_model_types();
        return raw<30 ? &s_cab_types[raw] : NULL;
    case BLOCK_DELAY:
        return find_type(DELAY_TYPES,
            sizeof(DELAY_TYPES)/sizeof(DELAY_TYPES[0]),raw);
    case BLOCK_MOD:
        return find_type(MOD_TYPES,
            sizeof(MOD_TYPES)/sizeof(MOD_TYPES[0]),raw);
    case BLOCK_REVERB:
        return find_type(REVERB_TYPES,
            sizeof(REVERB_TYPES)/sizeof(REVERB_TYPES[0]),raw);
    case BLOCK_EQ:
        return find_type(EQ_TYPES,
            sizeof(EQ_TYPES)/sizeof(EQ_TYPES[0]),raw);
    default:
        return NULL;
    }
}

size_t nanocore_spec_editor_type_count(controller_block_t block)
{
    switch (block) {
    case BLOCK_FX1: return nanocore_spec_fx1_selector_count();
    case BLOCK_FX2: return 21;
    case BLOCK_AMP:
    case BLOCK_CAB: return 30;
    case BLOCK_DELAY: return sizeof(DELAY_TYPES)/sizeof(DELAY_TYPES[0]);
    case BLOCK_MOD: return sizeof(MOD_SELECTOR_ORDER)/sizeof(MOD_SELECTOR_ORDER[0]);
    case BLOCK_REVERB: return sizeof(REVERB_SELECTOR_ORDER)/sizeof(REVERB_SELECTOR_ORDER[0]);
    case BLOCK_EQ: return sizeof(EQ_TYPES)/sizeof(EQ_TYPES[0]);
    default: return 0;
    }
}

const nanocore_effect_type_info_t *nanocore_spec_editor_type_by_selector(
    controller_block_t block,
    size_t index)
{
    switch (block) {
    case BLOCK_FX1:
        return nanocore_spec_fx1_selector(index);
    case BLOCK_FX2:
        return index<21
            ? nanocore_spec_editor_type_by_raw(block,(uint8_t)index)
            : NULL;
    case BLOCK_AMP:
    case BLOCK_CAB:
        return index<30
            ? nanocore_spec_editor_type_by_raw(block,(uint8_t)index)
            : NULL;
    case BLOCK_DELAY:
        return index<sizeof(DELAY_TYPES)/sizeof(DELAY_TYPES[0])
            ? &DELAY_TYPES[index] : NULL;
    case BLOCK_MOD:
        return index<sizeof(MOD_SELECTOR_ORDER)/sizeof(MOD_SELECTOR_ORDER[0])
            ? nanocore_spec_editor_type_by_raw(
                block,MOD_SELECTOR_ORDER[index]) : NULL;
    case BLOCK_REVERB:
        return index<sizeof(REVERB_SELECTOR_ORDER)/sizeof(REVERB_SELECTOR_ORDER[0])
            ? nanocore_spec_editor_type_by_raw(
                block,REVERB_SELECTOR_ORDER[index]) : NULL;
    case BLOCK_EQ:
        return index<sizeof(EQ_TYPES)/sizeof(EQ_TYPES[0])
            ? &EQ_TYPES[index] : NULL;
    default:
        return NULL;
    }
}

int nanocore_spec_editor_selector_index(
    controller_block_t block,
    uint8_t raw)
{
    size_t count=nanocore_spec_editor_type_count(block);
    for(size_t i=0;i<count;++i){
        const nanocore_effect_type_info_t *t=
            nanocore_spec_editor_type_by_selector(block,i);
        if(t && t->id==raw) return (int)i;
    }
    return -1;
}

bool nanocore_spec_editor_defaults(
    const nanocore_effect_type_info_t *type,
    float *out,
    size_t out_count)
{
    if(!type || !out ||
       out_count < type->expected_param_count ||
       type->expected_param_count > 9) {
        return false;
    }

    if(type->default_normalized){
        memcpy(out,type->default_normalized,
               (size_t)type->expected_param_count*sizeof(float));
        return true;
    }

    if(!type->params) return type->expected_param_count==0;

    for(uint8_t i=0;i<type->expected_param_count;++i){
        out[i]=nanocore_spec_real_to_normalized(
            &type->params[i],type->params[i].default_value);
    }
    return true;
}

void nanocore_spec_format_normalized(
    const nanocore_param_info_t *param,
    float normalized,
    char *out,
    size_t out_len)
{
    if(!out || out_len==0) return;
    out[0]='\0';

    if(!param){
        snprintf(out,out_len,"%.3f",(double)normalized);
        return;
    }

    if(param->value_kind==NC_PARAM_VALUE_BOOLEAN){
        snprintf(out,out_len,"%s",normalized>=0.5f?"ON":"OFF");
        return;
    }

    float real=nanocore_spec_normalized_to_real(param,normalized);

    if(param->value_kind==NC_PARAM_VALUE_ENUM &&
       param->options && param->option_count){
        int idx=(int)lroundf(real);
        if(idx<0) idx=0;
        if(idx>=param->option_count) idx=param->option_count-1;
        snprintf(out,out_len,"%s",param->options[idx]);
        return;
    }

    if(param->unit && strcmp(param->unit,":1")==0){
        snprintf(out,out_len,"%.*f:1",
                 param->decimals,(double)real);
    }else if(param->unit && param->unit[0]){
        snprintf(out,out_len,"%.*f %s",
                 param->decimals,(double)real,param->unit);
    }else{
        snprintf(out,out_len,"%.*f",
                 param->decimals,(double)real);
    }
}

float nanocore_spec_adjust_normalized(
    const nanocore_param_info_t *param,
    float current,
    int delta)
{
    if(!param || delta==0) return current;

    if(current<0.0f) current=0.0f;
    if(current>1.0f) current=1.0f;

    if(param->value_kind==NC_PARAM_VALUE_BOOLEAN){
        return delta>0 ? 1.0f : 0.0f;
    }

    if(param->value_kind==NC_PARAM_VALUE_ENUM &&
       param->option_count>1){
        float real=nanocore_spec_normalized_to_real(param,current);
        int idx=(int)lroundf(real)+delta;
        while(idx<0) idx+=param->option_count;
        while(idx>=param->option_count) idx-=param->option_count;
        return (float)idx/(float)(param->option_count-1);
    }

    float next=current+((float)delta*0.01f);
    if(next<0.0f) next=0.0f;
    if(next>1.0f) next=1.0f;
    return next;
}

/* Stage 08B: normalized vectors captured on the user's NanoCore fw 1.2.
 * Names, units and range/curve metadata follow the external wire-traffic
 * mapping and still require display comparison on this device. */
static const char *const SYNTH_WAVES[] = {"Sine","Saw","Square","Triangle","Pulse"};
static const float SYNTH_0_DEF[] = {0.250000f,0.080000f,0.500000f,0.250000f,0.680000f,1.000000f};
static const nanocore_param_info_t SYNTH_0_PARAMS[] = {
    P_ENUM("Wave", SYNTH_WAVES, 5, 1),
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=4.547200f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Detune","ct",0.0f,20.0f,1,5.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,68.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_1_DEF[] = {0.450000f,0.720000f,0.500000f,0.550000f,1.000000f};
static const nanocore_param_info_t SYNTH_1_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=82.595000f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,72.000000f),
    P_LINEAR("Ensemble","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,55.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_2_DEF[] = {1.000000f,0.500000f,1.000000f};
static const nanocore_param_info_t SYNTH_2_PARAMS[] = {
    P_LINEAR("Tone","",0.0f,100.0f,0,100.000000f),
    P_LINEAR("Decay","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_3_DEF[] = {0.550000f,0.700000f,0.200000f,0.600000f,1.000000f};
static const nanocore_param_info_t SYNTH_3_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=122.395000f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,70.000000f),
    P_LINEAR("Vowel","",0.0f,100.0f,0,20.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,60.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_4_DEF[] = {0.240000f,0.500000f,0.600000f,0.700000f,1.000000f};
static const nanocore_param_info_t SYNTH_4_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=24.924800f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Bite","",0.0f,100.0f,0,60.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,70.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_5_DEF[] = {0.040000f,0.250000f,0.600000f,0.850000f,1.000000f};
static const nanocore_param_info_t SYNTH_5_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=2.636800f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,25.000000f),
    P_LINEAR("Harmonics","",0.0f,100.0f,0,60.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,85.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_6_DEF[] = {0.310000f,0.450000f,0.350000f,0.700000f,1.000000f};
static const nanocore_param_info_t SYNTH_6_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=40.247800f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,45.000000f),
    P_LINEAR("Breath","",0.0f,100.0f,0,35.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,70.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_7_DEF[] = {1.000000f,0.500000f,1.000000f};
static const nanocore_param_info_t SYNTH_7_PARAMS[] = {
    P_LINEAR("Tone","",0.0f,100.0f,0,100.000000f),
    P_LINEAR("Decay","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_8_DEF[] = {0.000000f,0.300000f,0.700000f,0.400000f,1.000000f};
static const nanocore_param_info_t SYNTH_8_PARAMS[] = {
    { .name="Attack", .unit="ms", .min_value=2.0f, .mid_value=101.5f,
      .max_value=400.0f, .decimals=0, .curve=NC_PARAM_QUADRATIC,
      .default_value=2.000000f, .value_kind=NC_PARAM_VALUE_NUMERIC },
    P_LINEAR("Release","",0.0f,100.0f,0,30.000000f),
    P_LINEAR("Sub Level","",0.0f,100.0f,0,70.000000f),
    P_LINEAR("Tone","",0.0f,100.0f,0,40.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const float SYNTH_9_DEF[] = {1.000000f,0.500000f,1.000000f};
static const nanocore_param_info_t SYNTH_9_PARAMS[] = {
    P_LINEAR("Tone","",0.0f,100.0f,0,100.000000f),
    P_LINEAR("Decay","",0.0f,100.0f,0,50.000000f),
    P_LINEAR("Mix","",0.0f,100.0f,0,100.000000f),
};
static const nanocore_effect_type_info_t SYNTH_VOICES[] = {
    TYPE(0, "Analog Synth", SYNTH_0_PARAMS, SYNTH_0_DEF),
    TYPE(1, "Strings", SYNTH_1_PARAMS, SYNTH_1_DEF),
    TYPE(2, "Pad", SYNTH_2_PARAMS, SYNTH_2_DEF),
    TYPE(3, "Choir", SYNTH_3_PARAMS, SYNTH_3_DEF),
    TYPE(4, "Brass", SYNTH_4_PARAMS, SYNTH_4_DEF),
    TYPE(5, "Organ", SYNTH_5_PARAMS, SYNTH_5_DEF),
    TYPE(6, "Flute", SYNTH_6_PARAMS, SYNTH_6_DEF),
    TYPE(7, "Lead", SYNTH_7_PARAMS, SYNTH_7_DEF),
    TYPE(8, "Bass", SYNTH_8_PARAMS, SYNTH_8_DEF),
    TYPE(9, "Pluck", SYNTH_9_PARAMS, SYNTH_9_DEF),
};
size_t nanocore_spec_synth_voice_count(void)
{
    return sizeof(SYNTH_VOICES)/sizeof(SYNTH_VOICES[0]);
}
const nanocore_effect_type_info_t *nanocore_spec_synth_voice(uint8_t id)
{
    return id < nanocore_spec_synth_voice_count() ? &SYNTH_VOICES[id] : NULL;
}

/* Stage 08C: observed Synth catalog membership. Keep the legacy enum,
 * normal selector ordering, wire IDs and persistent structures unchanged.
 * REV sequence is the order observed in the physical sweep; confirm it
 * is the actual consecutive selector order before final UI acceptance. */
static const uint8_t SYNTH_REV_SELECTOR_ORDER[] = {0,1,2,3,7,5,6};

size_t nanocore_spec_mode_type_count(controller_block_t block, bool synth_mode)
{
    if (!synth_mode) return nanocore_spec_editor_type_count(block);
    switch (block) {
    case BLOCK_FX1:
    case BLOCK_DELAY:
    case BLOCK_MOD:
    case BLOCK_EQ:
        return nanocore_spec_editor_type_count(block);
    case BLOCK_REVERB:
        return sizeof(SYNTH_REV_SELECTOR_ORDER)/sizeof(SYNTH_REV_SELECTOR_ORDER[0]);
    case BLOCK_FX2: /* No physical Pitch..last sweep yet. */
    case BLOCK_AMP:
    case BLOCK_CAB:
    default:
        return 0;
    }
}

const nanocore_effect_type_info_t *nanocore_spec_mode_type_by_selector(
    controller_block_t block, bool synth_mode, size_t index)
{
    if (index >= nanocore_spec_mode_type_count(block, synth_mode)) return NULL;
    if (synth_mode && block == BLOCK_REVERB) {
        return nanocore_spec_editor_type_by_raw(block, SYNTH_REV_SELECTOR_ORDER[index]);
    }
    return nanocore_spec_editor_type_by_selector(block, index);
}

int nanocore_spec_mode_selector_index(
    controller_block_t block, bool synth_mode, uint8_t raw_type_id)
{
    size_t count = nanocore_spec_mode_type_count(block, synth_mode);
    for (size_t i = 0; i < count; ++i) {
        const nanocore_effect_type_info_t *type =
            nanocore_spec_mode_type_by_selector(block, synth_mode, i);
        if (type && type->id == raw_type_id) return (int)i;
    }
    return -1;
}
