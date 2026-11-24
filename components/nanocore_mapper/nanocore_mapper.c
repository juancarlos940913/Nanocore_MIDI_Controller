#include "nanocore_mapper.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controller_types.h"
#include "esp_log.h"
#include "esp_timer.h"

#define MAP_POLL_INTERVAL_MS  800
#define MAP_REPLY_TIMEOUT_MS 1400

static const char *TAG = "NC_MAP";

static bool s_enabled;
static bool s_inflight;
static int64_t s_next_poll_us;
static int64_t s_sent_us;
static uint32_t s_last_hash;

static const char *BLOCK_NAMES[BLOCK_COUNT] = {
    "FX1", "FX2", "AMP", "CAB", "DELAY", "MOD", "REVERB", "EQ"
};

static uint32_t fnv1a_u8(uint32_t h, uint8_t v)
{
    h ^= v;
    return h * 16777619u;
}

static uint32_t fnv1a_bytes(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) h = fnv1a_u8(h, p[i]);
    return h;
}

static uint32_t snapshot_hash(const nanocore_protocol_event_t *e)
{
    uint32_t h = 2166136261u;
    h = fnv1a_u8(h, e->slot);

    for (int b = 0; b < BLOCK_COUNT; ++b) {
        h = fnv1a_u8(h, e->block_profile_known[b] ? 1 : 0);
        h = fnv1a_u8(h, e->block_on[b] ? 1 : 0);
        h = fnv1a_u8(h, e->block_variant[b]);
        h = fnv1a_u8(h, e->block_param_count[b]);

        uint8_t count = e->block_param_count[b];
        if (count > NC_PROTO_MAX_PARAMS) count = NC_PROTO_MAX_PARAMS;
        h = fnv1a_bytes(h, e->block_params[b],
                        (size_t)count * sizeof(float));
    }

    h = fnv1a_u8(h, e->chain_order_known ? 1 : 0);
    if (e->chain_order_known) {
        h = fnv1a_bytes(h, e->chain_order, BLOCK_COUNT);
    }

    h = fnv1a_u8(h, e->synth_known ? 1 : 0);
    if (e->synth_known) {
        h = fnv1a_u8(h, e->synth_on ? 1 : 0);
        h = fnv1a_u8(h, e->synth_variant);
        h = fnv1a_u8(h, e->synth_param_count);
        uint8_t count = e->synth_param_count;
        if (count > NC_PROTO_SYNTH_MAX_PARAMS)
            count = NC_PROTO_SYNTH_MAX_PARAMS;
        h = fnv1a_bytes(h, e->synth_params,
                        (size_t)count * sizeof(float));
    }

    return h;
}

static void log_params_line(uint8_t slot,
                            const char *block,
                            bool on,
                            uint8_t variant,
                            uint8_t count,
                            const float *params)
{
    char line[360];
    int n = snprintf(line, sizeof(line),
                     "MAP|slot=%u|block=%s|on=%u|variant=%u|count=%u",
                     slot, block, on ? 1 : 0, variant, count);

    for (uint8_t i = 0;
         i < count && i < NC_PROTO_MAX_PARAMS && n > 0 &&
         (size_t)n < sizeof(line);
         ++i) {
        int w = snprintf(&line[n], sizeof(line) - (size_t)n,
                         "|p%u=%.6f", i, (double)params[i]);
        if (w < 0) break;
        n += w;
    }

    ESP_LOGI(TAG, "%s", line);
}

esp_err_t nanocore_mapper_init(void)
{
    s_enabled = false;
    s_inflight = false;
    s_next_poll_us = 0;
    s_sent_us = 0;
    s_last_hash = 0;

    ESP_LOGI(TAG,
             "Live Mapper ready; hold both TOP footswitches to toggle");
    return ESP_OK;
}

void nanocore_mapper_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_inflight = false;
    s_last_hash = 0;
    s_next_poll_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Live Mapper %s", enabled ? "ON" : "OFF");
}

bool nanocore_mapper_is_enabled(void)
{
    return s_enabled;
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

        ESP_LOGW(TAG, "Live mapper 0x63 timeout; retrying");
        s_inflight = false;
    }

    if (now < s_next_poll_us) return;

    esp_err_t rc = nanocore_protocol_request_live_state();
    if (rc == ESP_OK) {
        s_inflight = true;
        s_sent_us = now;
        s_next_poll_us =
            now + (MAP_POLL_INTERVAL_MS * 1000LL);
    } else {
        ESP_LOGW(TAG, "Mapper live-state request failed: %s",
                 esp_err_to_name(rc));
        s_next_poll_us =
            now + (MAP_POLL_INTERVAL_MS * 1000LL);
    }
}

void nanocore_mapper_on_live_state(const nanocore_protocol_event_t *e)
{
    s_inflight = false;

    if (!s_enabled || !e ||
        e->type != NC_PROTO_EVENT_LIVE_STATE) {
        return;
    }

    const uint32_t h = snapshot_hash(e);
    if (h == s_last_hash) return;
    s_last_hash = h;

    ESP_LOGI(TAG, "MAP|BEGIN|slot=%u|hash=%08lX",
             e->slot, (unsigned long)h);

    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!e->block_profile_known[b]) continue;

        log_params_line(
            e->slot,
            BLOCK_NAMES[b],
            e->block_on[b],
            e->block_variant[b],
            e->block_param_count[b],
            e->block_params[b]);
    }

    if (e->chain_order_known) {
        ESP_LOGI(TAG,
                 "MAP|slot=%u|chain=%u,%u,%u,%u,%u,%u,%u,%u",
                 e->slot,
                 e->chain_order[0], e->chain_order[1],
                 e->chain_order[2], e->chain_order[3],
                 e->chain_order[4], e->chain_order[5],
                 e->chain_order[6], e->chain_order[7]);
    }

    if (e->synth_known) {
        char line[360];
        int n = snprintf(line, sizeof(line),
                         "MAP|slot=%u|block=SYNTH|on=%u|variant=%u|count=%u",
                         e->slot, e->synth_on ? 1 : 0,
                         e->synth_variant, e->synth_param_count);

        for (uint8_t i = 0;
             i < e->synth_param_count &&
             i < NC_PROTO_SYNTH_MAX_PARAMS &&
             n > 0 && (size_t)n < sizeof(line);
             ++i) {
            int w = snprintf(&line[n], sizeof(line) - (size_t)n,
                             "|p%u=%.6f",
                             i, (double)e->synth_params[i]);
            if (w < 0) break;
            n += w;
        }

        ESP_LOGI(TAG, "%s", line);
    }

    ESP_LOGI(TAG, "MAP|END|slot=%u", e->slot);
}
