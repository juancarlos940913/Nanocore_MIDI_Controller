#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "controller_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void tempo_engine_init(uint16_t bpm);
void tempo_engine_set_bpm(uint16_t bpm);
uint16_t tempo_engine_get_bpm(void);

void tempo_engine_begin_tap(void);
bool tempo_engine_tap(void);
bool tempo_engine_tap_active(void);
void tempo_engine_cancel_tap(void);
void tempo_engine_service(void);

uint32_t tempo_engine_delay_ms(uint16_t bpm,
                               delay_subdivision_t subdivision);

const char *tempo_engine_subdivision_name(delay_subdivision_t subdivision);

/*
 * NanoCore publishes CC74 as a 0..127 Delay/Time parameter but does not
 * publish the milliseconds transfer function. This conversion is kept
 * deliberately isolated so it can be calibrated later without touching UI.
 */
uint8_t tempo_engine_delay_ms_to_cc74(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
