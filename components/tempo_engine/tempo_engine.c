#include "tempo_engine.h"

#include <stddef.h>

#include "esp_timer.h"

#define BPM_MIN  30
#define BPM_MAX 300
#define TAP_MIN_INTERVAL_MS 180
#define TAP_MAX_INTERVAL_MS 2000
#define TAP_TIMEOUT_MS      4500

// Provisional calibration endpoints for CC74.
// The public NanoCore guide does not document its ms curve.
#define DELAY_CAL_MIN_MS      20
#define DELAY_CAL_MAX_MS    2000

static uint16_t s_bpm = 120;
static bool s_tap_active;
static int64_t s_last_tap_us;
static uint32_t s_intervals[4];
static uint8_t s_interval_count;

static uint16_t clamp_bpm(uint16_t bpm)
{
    if (bpm < BPM_MIN) return BPM_MIN;
    if (bpm > BPM_MAX) return BPM_MAX;
    return bpm;
}

void tempo_engine_init(uint16_t bpm)
{
    s_bpm = clamp_bpm(bpm);
    s_tap_active = false;
    s_last_tap_us = 0;
    s_interval_count = 0;
}

void tempo_engine_set_bpm(uint16_t bpm)
{
    s_bpm = clamp_bpm(bpm);
}

uint16_t tempo_engine_get_bpm(void)
{
    return s_bpm;
}

void tempo_engine_begin_tap(void)
{
    s_tap_active = true;
    s_last_tap_us = 0;
    s_interval_count = 0;
}

bool tempo_engine_tap(void)
{
    int64_t now = esp_timer_get_time();

    if (!s_tap_active) {
        tempo_engine_begin_tap();
    }

    if (s_last_tap_us != 0) {
        uint32_t interval = (uint32_t)((now - s_last_tap_us) / 1000);

        if (interval >= TAP_MIN_INTERVAL_MS &&
            interval <= TAP_MAX_INTERVAL_MS) {

            if (s_interval_count < 4) {
                s_intervals[s_interval_count++] = interval;
            } else {
                for (int i = 0; i < 3; ++i) {
                    s_intervals[i] = s_intervals[i + 1];
                }
                s_intervals[3] = interval;
            }

            uint32_t sum = 0;
            for (uint8_t i = 0; i < s_interval_count; ++i) {
                sum += s_intervals[i];
            }
            uint32_t avg = sum / s_interval_count;
            if (avg > 0) {
                s_bpm = clamp_bpm((uint16_t)(60000UL / avg));
            }
        } else {
            s_interval_count = 0;
        }
    }

    s_last_tap_us = now;
    return s_interval_count > 0;
}

bool tempo_engine_tap_active(void)
{
    return s_tap_active;
}

void tempo_engine_cancel_tap(void)
{
    s_tap_active = false;
    s_last_tap_us = 0;
    s_interval_count = 0;
}

void tempo_engine_service(void)
{
    if (!s_tap_active || s_last_tap_us == 0) return;

    int64_t idle_ms = (esp_timer_get_time() - s_last_tap_us) / 1000;
    if (idle_ms > TAP_TIMEOUT_MS) {
        tempo_engine_cancel_tap();
    }
}

uint32_t tempo_engine_delay_ms(uint16_t bpm,
                               delay_subdivision_t subdivision)
{
    bpm = clamp_bpm(bpm);
    const uint32_t quarter_x1000 = (60000UL * 1000UL) / bpm;
    uint32_t multiplier_x1000 = 1000;

    switch (subdivision) {
    case DELAY_SUBDIV_HALF:           multiplier_x1000 = 2000; break;
    case DELAY_SUBDIV_QUARTER:        multiplier_x1000 = 1000; break;
    case DELAY_SUBDIV_EIGHTH:         multiplier_x1000 =  500; break;
    case DELAY_SUBDIV_SIXTEENTH:      multiplier_x1000 =  250; break;
    case DELAY_SUBDIV_DOTTED_QUARTER: multiplier_x1000 = 1500; break;
    case DELAY_SUBDIV_DOTTED_EIGHTH:  multiplier_x1000 =  750; break;
    default:                           multiplier_x1000 = 1000; break;
    }

    return (quarter_x1000 * multiplier_x1000) / 1000000UL;
}

const char *tempo_engine_subdivision_name(delay_subdivision_t s)
{
    switch (s) {
    case DELAY_SUBDIV_HALF:           return "1/2";
    case DELAY_SUBDIV_QUARTER:        return "1/4";
    case DELAY_SUBDIV_EIGHTH:         return "1/8";
    case DELAY_SUBDIV_SIXTEENTH:      return "1/16";
    case DELAY_SUBDIV_DOTTED_QUARTER: return "1/4.";
    case DELAY_SUBDIV_DOTTED_EIGHTH:  return "1/8.";
    default:                           return "?";
    }
}

uint8_t tempo_engine_delay_ms_to_cc74(uint32_t ms)
{
    if (ms <= DELAY_CAL_MIN_MS) return 0;
    if (ms >= DELAY_CAL_MAX_MS) return 127;

    uint32_t span = DELAY_CAL_MAX_MS - DELAY_CAL_MIN_MS;
    uint32_t pos = ms - DELAY_CAL_MIN_MS;
    return (uint8_t)((pos * 127UL + span / 2) / span);
}
