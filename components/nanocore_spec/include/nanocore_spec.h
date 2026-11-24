#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t id;
    const char *name;
    uint8_t expected_param_count;
} nanocore_effect_type_info_t;

/*
 * FX1 catalog for current NanoCore firmware generation.
 *
 * IDs 0..3 come from the official MIDI/user documentation.
 * IDs 7..11 are the five effects explicitly listed in firmware 1.2 release
 * notes. IDs 4..6 (Env Wah / Wah / Motion Wah) and their wire ids are based
 * on independently captured ToneCommand traffic and are now corroborated by
 * the user's live 0x63 sweep: all 12 ids and parameter counts match.
 */
const nanocore_effect_type_info_t *nanocore_spec_fx1_type(uint8_t type_id);

#ifdef __cplusplus
}
#endif
