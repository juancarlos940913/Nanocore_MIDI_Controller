#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "nanocore_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nanocore_mapper_init(void);

void nanocore_mapper_set_enabled(bool enabled);
bool nanocore_mapper_is_enabled(void);

/*
 * Poll the NanoCore live profile only while mapper mode is enabled.
 * Set transport_busy while a Program Change recall is being synchronized,
 * so mapper traffic never overlaps the slot-confirmation state machine.
 */
void nanocore_mapper_service(bool midi_ready, bool transport_busy);

/*
 * Feed each accepted 0x63 live-state event. Mapper logs only snapshots whose
 * device-state hash changed, keeping serial captures compact.
 */
void nanocore_mapper_on_live_state(const nanocore_protocol_event_t *event);

#ifdef __cplusplus
}
#endif
