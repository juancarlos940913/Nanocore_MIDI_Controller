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
 * Queue one explicit live-profile capture. Stage 06.2 intentionally avoids
 * continuous polling because a long characterization session can exhaust the
 * BLE write-no-response mbuf pool.
 */
void nanocore_mapper_request_capture(void);

/*
 * Service a queued capture only while mapper mode is enabled.
 * Set transport_busy while Program Change recall is being synchronized so
 * diagnostic traffic never overlaps the slot-confirmation state machine.
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
