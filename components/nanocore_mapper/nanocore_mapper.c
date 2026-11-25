#include "nanocore_mapper.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controller_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nanocore_spec.h"

#define MAP_REPLY_TIMEOUT_MS 2500

static const char *TAG = "NC_MAP";

static bool s_enabled;
static bool s_inflight;
static bool s_capture_pending;
static int64_t s_sent_us;

static controller_block_t s_target = BLOCK_FX1;

static bool s_seen[128];
static uint8_t s_unique;
static uint8_t s_match;
static uint8_t s_mismatch;
static uint8_t s_unknown;

static bool s_have_prev;
static uint8_t s_prev_variant;
static uint8_t s_prev_count;
static float s_prev_params[NC_PROTO_MAX_PARAMS];

static const char *BLOCK_NAMES[BLOCK_COUNT] = {
    "FX1", "FX2", "AMP", "CAB", "DELAY", "MOD", "REVERB", "EQ"
};

static void reset_session(void)
{
    memset(s_seen, 0, sizeof(s_seen));
    s_unique = 0;
    s_match = 0;
    s_mismatch = 0;
    s_unknown = 0;

    s_have_prev = false;
    s_prev_variant = 0;
    s_prev_count = 0;
    memset(s_prev_params, 0, sizeof(s_prev_params));
}

static void log_summary(void)
{
    const size_t expected =
        nanocore_spec_candidate_count((uint8_t)s_target);

    ESP_LOGI(TAG,
             "ASSIST|SUMMARY|block=%s|unique=%u|match=%u|mismatch=%u|unknown=%u|candidate_types=%u",
             BLOCK_NAMES[s_target],
             s_unique, s_match, s_mismatch, s_unknown,
             (unsigned)expected);

    if (s_target == BLOCK_AMP || s_target == BLOCK_CAB) {
        ESP_LOGI(TAG,
                 "ASSIST|SUMMARY|block=%s|status=PROFILE_ONLY|note=0x63 raw variant is not authoritative model id",
                 BLOCK_NAMES[s_target]);
    }
}

static void log_raw_params(const nanocore_protocol_event_t *e)
{
    const int b = (int)s_target;
    uint8_t count = e->block_param_count[b];
    if (count > NC_PROTO_MAX_PARAMS) count = NC_PROTO_MAX_PARAMS;

    char line[360];
    int n = snprintf(line, sizeof(line),
                     "ASSIST|RAW|block=%s|variant=%u|count=%u",
                     BLOCK_NAMES[b],
                     e->block_variant[b],
                     count);

    for (uint8_t i = 0;
         i < count && n > 0 && (size_t)n < sizeof(line);
         ++i) {
        int w = snprintf(&line[n], sizeof(line) - (size_t)n,
                         "|p%u=%.6f",
                         i, (double)e->block_params[b][i]);
        if (w < 0) break;
        n += w;
    }

    ESP_LOGI(TAG, "%s", line);
}

static void log_diffs(const nanocore_protocol_event_t *e)
{
    if (!s_have_prev) return;

    const int b = (int)s_target;
    const uint8_t variant = e->block_variant[b];
    uint8_t count = e->block_param_count[b];
    if (count > NC_PROTO_MAX_PARAMS) count = NC_PROTO_MAX_PARAMS;

    if (variant != s_prev_variant || count != s_prev_count) return;

    for (uint8_t i = 0; i < count; ++i) {
        float oldv = s_prev_params[i];
        float newv = e->block_params[b][i];
        float d = newv - oldv;
        if (d < 0.0f) d = -d;
        if (d <= 0.0005f) continue;

        ESP_LOGI(TAG,
                 "ASSIST|DIFF|block=%s|variant=%u|p%u=%.6f->%.6f",
                 BLOCK_NAMES[b], variant, i,
                 (double)oldv, (double)newv);
    }
}

static void save_prev(const nanocore_protocol_event_t *e)
{
    const int b = (int)s_target;
    s_prev_variant = e->block_variant[b];
    s_prev_count = e->block_param_count[b];
    if (s_prev_count > NC_PROTO_MAX_PARAMS) {
        s_prev_count = NC_PROTO_MAX_PARAMS;
    }

    memset(s_prev_params, 0, sizeof(s_prev_params));
    memcpy(s_prev_params, e->block_params[b],
           (size_t)s_prev_count * sizeof(float));
    s_have_prev = true;
}

esp_err_t nanocore_mapper_init(void)
{
    s_enabled = false;
    s_inflight = false;
    s_capture_pending = false;
    s_sent_us = 0;
    s_target = BLOCK_FX1;
    reset_session();

    ESP_LOGI(TAG,
             "Assisted Mapper ready; select block, TOP+TOP toggles, ENC2 captures");
    return ESP_OK;
}

void nanocore_mapper_set_target(controller_block_t block)
{
    if (block >= BLOCK_COUNT) return;
    if (s_enabled) {
        ESP_LOGW(TAG, "Cannot change mapper target while active");
        return;
    }
    s_target = block;
}

controller_block_t nanocore_mapper_target(void)
{
    return s_target;
}

void nanocore_mapper_set_enabled(bool enabled)
{
    if (enabled == s_enabled) return;

    if (!enabled) {
        log_summary();
        s_enabled = false;
        s_inflight = false;
        s_capture_pending = false;
        ESP_LOGI(TAG, "Assisted Mapper OFF");
        return;
    }

    s_enabled = true;
    s_inflight = false;
    s_capture_pending = true;   // baseline
    reset_session();

    ESP_LOGI(TAG,
             "ASSIST|START|block=%s|candidate_types=%u|mode=manual_capture",
             BLOCK_NAMES[s_target],
             (unsigned)nanocore_spec_candidate_count((uint8_t)s_target));

    if (s_target == BLOCK_AMP || s_target == BLOCK_CAB) {
        ESP_LOGW(TAG,
                 "ASSIST|block=%s|status=PROFILE_ONLY|reason=model_variant_not_authoritative",
                 BLOCK_NAMES[s_target]);
    }

    ESP_LOGI(TAG, "Assisted Mapper ON - ENC2 CAPTURE");
}

bool nanocore_mapper_is_enabled(void)
{
    return s_enabled;
}

void nanocore_mapper_request_capture(void)
{
    if (!s_enabled) return;

    if (s_inflight) {
        ESP_LOGW(TAG, "Mapper capture already in flight");
        return;
    }

    s_capture_pending = true;
    ESP_LOGI(TAG, "Manual capture queued for %s",
             BLOCK_NAMES[s_target]);
}

void nanocore_mapper_service(bool midi_ready, bool transport_busy)
{
    if (!s_enabled || !midi_ready || transport_busy) return;

    const int64_t now = esp_timer_get_time();

    if (s_inflight) {
        if ((now - s_sent_us) <
            (MAP_REPLY_TIMEOUT_MS * 1000LL)) {
            return;
        }

        ESP_LOGW(TAG, "Manual mapper capture timed out");
        s_inflight = false;
        return;
    }

    if (!s_capture_pending) return;

    s_capture_pending = false;

    esp_err_t rc = nanocore_protocol_request_live_state();
    if (rc == ESP_OK) {
        s_inflight = true;
        s_sent_us = now;
        ESP_LOGI(TAG, "Manual 0x63 capture requested for %s",
                 BLOCK_NAMES[s_target]);
    } else {
        ESP_LOGW(TAG, "Mapper capture request failed: %s",
                 esp_err_to_name(rc));
    }
}

void nanocore_mapper_on_live_state(const nanocore_protocol_event_t *e)
{
    s_inflight = false;

    if (!s_enabled || !e ||
        e->type != NC_PROTO_EVENT_LIVE_STATE) {
        return;
    }

    const int b = (int)s_target;
    if (!e->block_profile_known[b]) {
        ESP_LOGW(TAG,
                 "ASSIST|block=%s|status=NO_PROFILE",
                 BLOCK_NAMES[b]);
        return;
    }

    const uint8_t variant = e->block_variant[b];
    const uint8_t got_count = e->block_param_count[b];

    if (s_target == BLOCK_AMP || s_target == BLOCK_CAB) {
        ESP_LOGI(TAG,
                 "ASSIST|CAPTURE|block=%s|raw_variant=%u|count=%u|status=PROFILE_ONLY",
                 BLOCK_NAMES[b], variant, got_count);
        log_raw_params(e);
        log_diffs(e);
        save_prev(e);
        return;
    }

    const nanocore_candidate_type_t *candidate =
        nanocore_spec_candidate_type((uint8_t)s_target, variant);

    const char *status;
    if (!candidate) {
        status = "UNKNOWN";
    } else if (candidate->expected_param_count == got_count) {
        status = "MATCH";
    } else {
        status = "MISMATCH";
    }

    ESP_LOGI(TAG,
             "ASSIST|CAPTURE|block=%s|raw=%u|candidate=%s|expected_count=%d|got_count=%u|status=%s",
             BLOCK_NAMES[b],
             variant,
             candidate ? candidate->name : "UNKNOWN",
             candidate ? (int)candidate->expected_param_count : -1,
             got_count,
             status);

    if (candidate && candidate->parameter_names) {
        ESP_LOGI(TAG,
                 "ASSIST|CANDIDATE_PARAMS|block=%s|raw=%u|%s",
                 BLOCK_NAMES[b],
                 variant,
                 candidate->parameter_names);
    }

    log_raw_params(e);
    log_diffs(e);

    if (variant < 128 && !s_seen[variant]) {
        s_seen[variant] = true;
        ++s_unique;

        if (!candidate) {
            ++s_unknown;
        } else if (candidate->expected_param_count == got_count) {
            ++s_match;
        } else {
            ++s_mismatch;
        }
    }

    save_prev(e);
}
