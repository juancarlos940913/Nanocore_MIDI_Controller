#include "nanocore_mapper.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controller_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nanocore_spec.h"

#define MAP_POLL_INTERVAL_MS  800
#define MAP_REPLY_TIMEOUT_MS 1400
#define MAP_FLOAT_EPSILON 0.0005f

static const char *TAG = "NC_MAP";

static bool s_enabled;
static bool s_inflight;
static int64_t s_next_poll_us;
static int64_t s_sent_us;
static uint32_t s_last_hash;

static bool s_have_prev;
static nanocore_protocol_event_t s_prev;

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

    return h;
}

static const char *type_name_for(int block, uint8_t variant)
{
    if (block == BLOCK_FX1) {
        const nanocore_effect_type_info_t *info =
            nanocore_spec_fx1_type(variant);
        return info ? info->name : "Unknown";
    }
    return "?";
}

static bool float_changed(float a, float b)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d > MAP_FLOAT_EPSILON;
}

static void log_block_snapshot(uint8_t slot,
                               int block,
                               const nanocore_protocol_event_t *e)
{
    char line[420];
    const uint8_t variant = e->block_variant[block];
    const uint8_t count = e->block_param_count[block];
    const char *type_name = type_name_for(block, variant);

    int n = snprintf(line, sizeof(line),
                     "MAP|slot=%u|block=%s|on=%u|variant=%u|type=%s|count=%u",
                     slot, BLOCK_NAMES[block],
                     e->block_on[block] ? 1 : 0,
                     variant, type_name, count);

    const nanocore_effect_type_info_t *fx1_info =
        block == BLOCK_FX1 ? nanocore_spec_fx1_type(variant) : 0;

    for (uint8_t i = 0;
         i < count && i < NC_PROTO_MAX_PARAMS &&
         n > 0 && (size_t)n < sizeof(line);
         ++i) {

        if (fx1_info && fx1_info->params &&
            i < fx1_info->expected_param_count &&
            fx1_info->params[i].name) {
            const nanocore_param_info_t *p = &fx1_info->params[i];
            float real = nanocore_spec_normalized_to_real(
                p, e->block_params[block][i]);

            int w = snprintf(&line[n], sizeof(line) - (size_t)n,
                             "|p%u=%s:%.6f=>%.*f%s",
                             i, p->name,
                             (double)e->block_params[block][i],
                             p->decimals,
                             (double)real,
                             p->unit ? p->unit : "");
            if (w < 0) break;
            n += w;
        } else {
            int w = snprintf(&line[n], sizeof(line) - (size_t)n,
                             "|p%u=%.6f", i,
                             (double)e->block_params[block][i]);
            if (w < 0) break;
            n += w;
        }
    }

    ESP_LOGI(TAG, "%s", line);
}

static bool block_changed(const nanocore_protocol_event_t *a,
                          const nanocore_protocol_event_t *b,
                          int block)
{
    if (a->block_profile_known[block] != b->block_profile_known[block]) {
        return true;
    }
    if (!a->block_profile_known[block]) return false;

    if (a->block_on[block] != b->block_on[block] ||
        a->block_variant[block] != b->block_variant[block] ||
        a->block_param_count[block] != b->block_param_count[block]) {
        return true;
    }

    uint8_t count = a->block_param_count[block];
    if (count > NC_PROTO_MAX_PARAMS) count = NC_PROTO_MAX_PARAMS;

    for (uint8_t i = 0; i < count; ++i) {
        if (float_changed(a->block_params[block][i],
                          b->block_params[block][i])) {
            return true;
        }
    }
    return false;
}

static void log_block_diff(uint8_t slot,
                           const nanocore_protocol_event_t *prev,
                           const nanocore_protocol_event_t *now,
                           int block)
{
    const uint8_t old_variant = prev->block_variant[block];
    const uint8_t new_variant = now->block_variant[block];

    if (old_variant != new_variant ||
        prev->block_param_count[block] != now->block_param_count[block]) {
        ESP_LOGI(TAG,
                 "DIFF|slot=%u|block=%s|type_change=%u->%u|type=%s|count=%u",
                 slot, BLOCK_NAMES[block],
                 old_variant, new_variant,
                 type_name_for(block, new_variant),
                 now->block_param_count[block]);

        log_block_snapshot(slot, block, now);
        return;
    }

    if (prev->block_on[block] != now->block_on[block]) {
        ESP_LOGI(TAG,
                 "DIFF|slot=%u|block=%s|on=%u->%u",
                 slot, BLOCK_NAMES[block],
                 prev->block_on[block] ? 1 : 0,
                 now->block_on[block] ? 1 : 0);
    }

    uint8_t count = now->block_param_count[block];
    if (count > NC_PROTO_MAX_PARAMS) count = NC_PROTO_MAX_PARAMS;

    for (uint8_t i = 0; i < count; ++i) {
        if (!float_changed(prev->block_params[block][i],
                           now->block_params[block][i])) {
            continue;
        }

        const nanocore_effect_type_info_t *fx1_info =
            block == BLOCK_FX1 ?
            nanocore_spec_fx1_type(new_variant) : 0;

        if (fx1_info && fx1_info->params &&
            i < fx1_info->expected_param_count &&
            fx1_info->params[i].name) {
            const nanocore_param_info_t *p = &fx1_info->params[i];
            float old_real = nanocore_spec_normalized_to_real(
                p, prev->block_params[block][i]);
            float new_real = nanocore_spec_normalized_to_real(
                p, now->block_params[block][i]);

            ESP_LOGI(TAG,
                     "DIFF|slot=%u|block=%s|type=%s|variant=%u|param=%s|norm=%.6f->%.6f|real=%.*f%s->%.*f%s",
                     slot, BLOCK_NAMES[block],
                     type_name_for(block, new_variant),
                     new_variant, p->name,
                     (double)prev->block_params[block][i],
                     (double)now->block_params[block][i],
                     p->decimals, (double)old_real, p->unit ? p->unit : "",
                     p->decimals, (double)new_real, p->unit ? p->unit : "");
        } else {
            ESP_LOGI(TAG,
                     "DIFF|slot=%u|block=%s|type=%s|variant=%u|param=p%u|%.6f->%.6f",
                     slot, BLOCK_NAMES[block],
                     type_name_for(block, new_variant),
                     new_variant, i,
                     (double)prev->block_params[block][i],
                     (double)now->block_params[block][i]);
        }
    }
}

esp_err_t nanocore_mapper_init(void)
{
    s_enabled = false;
    s_inflight = false;
    s_next_poll_us = 0;
    s_sent_us = 0;
    s_last_hash = 0;
    s_have_prev = false;
    memset(&s_prev, 0, sizeof(s_prev));

    ESP_LOGI(TAG,
             "Live Mapper ready; hold both TOP footswitches to toggle");
    return ESP_OK;
}

void nanocore_mapper_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_inflight = false;
    s_last_hash = 0;
    s_have_prev = false;
    memset(&s_prev, 0, sizeof(s_prev));
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

    if (!s_have_prev) {
        ESP_LOGI(TAG, "MAP|BEGIN|slot=%u|hash=%08lX",
                 e->slot, (unsigned long)h);

        for (int b = 0; b < BLOCK_COUNT; ++b) {
            if (e->block_profile_known[b]) {
                log_block_snapshot(e->slot, b, e);
            }
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
            ESP_LOGI(TAG,
                     "MAP|slot=%u|synth_profile=stored|variant=%u|count=%u",
                     e->slot, e->synth_variant, e->synth_param_count);
        }

        ESP_LOGI(TAG, "MAP|END|slot=%u", e->slot);
    } else {
        for (int b = 0; b < BLOCK_COUNT; ++b) {
            if (block_changed(&s_prev, e, b)) {
                log_block_diff(e->slot, &s_prev, e, b);
            }
        }

        if (s_prev.chain_order_known != e->chain_order_known ||
            (e->chain_order_known &&
             memcmp(s_prev.chain_order, e->chain_order,
                    sizeof(e->chain_order)) != 0)) {
            if (e->chain_order_known) {
                ESP_LOGI(TAG,
                         "DIFF|slot=%u|chain=%u,%u,%u,%u,%u,%u,%u,%u",
                         e->slot,
                         e->chain_order[0], e->chain_order[1],
                         e->chain_order[2], e->chain_order[3],
                         e->chain_order[4], e->chain_order[5],
                         e->chain_order[6], e->chain_order[7]);
            }
        }
    }

    memcpy(&s_prev, e, sizeof(s_prev));
    s_have_prev = true;
}
