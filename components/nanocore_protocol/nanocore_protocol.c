/*
 * NanoCore proprietary SysEx interoperability layer.
 *
 * This implements only commands captured from the official ToneCommand app
 * and independently verified against real NanoCore hardware:
 *   0x68 heartbeat
 *   0x63 live-state read
 *   0x41 preset-name read window
 *   0x6D / field 0x08 rename active preset
 *   0x46 save active preset
 *
 * Public MIDI CC remains in nanocore_midi/nanocore_map.
 */

#include "nanocore_protocol.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nanocore_midi.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NC_MFR                 0x7D
#define NC_TAG_N               0x4E
#define NC_TAG_C               0x43
#define NC_DIR_HOST            0x70
#define NC_DIR_REPLY           0x71
#define NC_DIR_PUSH            0x72

#define OP_ACTIVE_SLOT         0x01
#define OP_BLOCK_STATE         0x02
#define OP_PARAM_VALUE         0x06
#define OP_AMP_STORAGE_INFO    0x30
#define OP_AMP_CATALOG         0x36
#define OP_READ_PRESET         0x41
#define OP_SAVE_PRESET         0x46
#define OP_CAB_STORAGE_INFO    0x50
#define OP_CAB_CATALOG         0x56
#define OP_LIVE_STATE          0x63
#define OP_GLOBAL_READ         0x65
#define OP_GLOBAL_SET          0x66
#define OP_GLOBAL_PUSH         0x07
#define OP_PING                0x68
#define OP_SET_FIELD           0x6D
#define FIELD_AMP_MODEL        0x06
#define FIELD_CAB_MODEL        0x07
#define FIELD_PRESET_NAME      0x08
#define FIELD_SYNTH_MODE       0x0A
#define WIRE_SYNTH_BLOCK       10

#define SEG_MORE               0x10
#define SEG_LAST               0x11
#define CATALOG_RECORD_LEN       30
#define CATALOG_AMP_RECORD_LEN   31
#define CATALOG_RECORD_MAX       CATALOG_AMP_RECORD_LEN
#define CATALOG_NAME_OFFSET      14

#define PROTO_EVENT_QUEUE      16
#define REASSEMBLY_MAX       2048
#define PING_INTERVAL_MS     3000
#define RENAME_SETTLE_MS       55
#define SAVE_RETRIES            3

static const char *TAG = "NC_PROTO";
static bool s_level_diagnostics;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_save_ack;
static TaskHandle_t s_ping_task;

/*
 * nanocore_protocol_event_t became intentionally large in Stage 06 because
 * a LIVE event carries every block's float parameters. Do NOT place it on
 * the BLE/system event task stack. The BLE callback path is serialized, so
 * one static scratch object is sufficient; queue_event() copies it into the
 * FreeRTOS queue before the next parse reuses it.
 */
static nanocore_protocol_event_t s_event_scratch;

static volatile bool s_connected;
static uint8_t s_seq;
static portMUX_TYPE s_seq_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_synth_capture_seq;
static volatile uint8_t s_wait_save_slot = 0xFF;

typedef struct {
    bool active;
    uint16_t seq;
    uint16_t opcode;
    uint16_t total;
    uint16_t received;
    uint8_t data[REASSEMBLY_MAX];
} reassembly_t;

static reassembly_t s_reassembly;

/* Dedicated capture mailbox. RX copies the complete payload while holding
 * the lock; only the main task formats/logs it from its own static copy.
 * Never retain pointers into the mutable BLE reassembly buffer. */
static portMUX_TYPE s_capture_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool ready;
    uint16_t seq;
    size_t len;
    uint8_t data[REASSEMBLY_MAX];
} s_capture_mailbox;
static uint8_t s_capture_print_buffer[REASSEMBLY_MAX];

/* Catalog records are short, and must never replace the in-progress
 * 0x63/0x41 reassembly if a user action arrives during the background read. */
static struct {
    bool active;
    uint16_t seq;
    uint16_t opcode;
    uint16_t received;
    uint16_t total;
    uint8_t data[CATALOG_RECORD_MAX];
} s_catalog_reply;

static float read_float32_le(const uint8_t *p)
{
    float value = 0.0f;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint8_t next_seq(void)
{
    portENTER_CRITICAL(&s_seq_lock);
    s_seq = (uint8_t)((s_seq + 1) & 0x7F);
    if (s_seq == 0) s_seq = 1;
    uint8_t seq = s_seq;
    portEXIT_CRITICAL(&s_seq_lock);
    return seq;
}

static nanocore_protocol_event_t *scratch_event(
    nanocore_protocol_event_type_t type)
{
    memset(&s_event_scratch, 0, sizeof(s_event_scratch));
    s_event_scratch.type = type;
    return &s_event_scratch;
}

static void queue_event(const nanocore_protocol_event_t *e)
{
    if (s_events && e) {
        (void)xQueueSend(s_events, e, 0);
    }
}

static size_t pack7(const uint8_t *in, size_t len,
                    uint8_t *out, size_t out_cap)
{
    size_t src = 0, dst = 0;
    while (src < len) {
        size_t chunk = len - src;
        if (chunk > 7) chunk = 7;
        if (dst + 1 + chunk > out_cap) return 0;

        uint8_t header = 0;
        for (size_t i = 0; i < chunk; ++i) {
            if (in[src + i] & 0x80) header |= (uint8_t)(1u << i);
        }
        out[dst++] = header;
        for (size_t i = 0; i < chunk; ++i) {
            out[dst++] = in[src + i] & 0x7F;
        }
        src += chunk;
    }
    return dst;
}

static size_t unpack7(const uint8_t *in, size_t len,
                      uint8_t *out, size_t out_cap)
{
    size_t src = 0, dst = 0;
    while (src < len) {
        uint8_t header = in[src++];
        for (int i = 0; i < 7 && src < len; ++i) {
            if (dst >= out_cap) return 0;
            out[dst++] = (uint8_t)(in[src++] |
                         (((header >> i) & 1u) << 7));
        }
    }
    return dst;
}

static esp_err_t send_short_command(uint8_t opcode)
{
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C,
        NC_DIR_HOST, 0x00, 0x02, next_seq(), 0x00,
        opcode, 0x00, 0x00, 0x00, 0xF7
    };
    return nanocore_midi_send_sysex(msg, sizeof(msg));
}

static esp_err_t send_type_prime(void)
{
    /*
     * ToneCommand sends this field-0 prime immediately before every
     * set-block-type command.
     */
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C,
        NC_DIR_HOST, 0x00, 0x02, next_seq(), 0x00,
        OP_SET_FIELD, 0x00, 0x01, 0x00, 0x00,
        0x00, 0xF7
    };
    return nanocore_midi_send_sysex(msg, sizeof(msg));
}

static esp_err_t send_scalar_field(uint8_t field_id, uint8_t value)
{
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C,
        NC_DIR_HOST, 0x00, 0x02, next_seq(), 0x00,
        OP_SET_FIELD, 0x00, 0x02, 0x00, 0x00,
        (uint8_t)(field_id & 0x7F),
        (uint8_t)(value & 0x7F),
        0xF7
    };

    return nanocore_midi_send_sysex(msg, sizeof(msg));
}

static esp_err_t send_block_type(uint8_t block,
                                 uint8_t type_id,
                                 const float *defaults,
                                 uint8_t count)
{
    if ((block >= BLOCK_COUNT && block != WIRE_SYNTH_BLOCK) ||
        count > NC_PROTO_MAX_PARAMS ||
        (count > 0 && !defaults)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t logical[4 + NC_PROTO_MAX_PARAMS * 4] = {0};
    size_t logical_len = 0;

    logical[logical_len++] = 0x02;
    logical[logical_len++] = (uint8_t)block;
    logical[logical_len++] = type_id & 0x7F;
    logical[logical_len++] = count & 0x7F;

    for (uint8_t i = 0; i < count; ++i) {
        uint8_t raw[sizeof(float)] = {0};
        memcpy(raw, &defaults[i], sizeof(raw));
        memcpy(&logical[logical_len], raw, sizeof(raw));
        logical_len += sizeof(raw);
    }

    uint8_t packed[64] = {0};
    size_t packed_len = pack7(logical, logical_len,
                              packed, sizeof(packed));
    if (!packed_len) return ESP_FAIL;

    uint8_t msg[96] = {0};
    size_t pos = 0;
    msg[pos++] = 0xF0;
    msg[pos++] = NC_MFR;
    msg[pos++] = NC_TAG_N;
    msg[pos++] = NC_TAG_C;
    msg[pos++] = NC_DIR_HOST;
    msg[pos++] = 0x00;
    msg[pos++] = 0x02;
    msg[pos++] = next_seq();
    msg[pos++] = 0x00;
    msg[pos++] = OP_SET_FIELD;
    msg[pos++] = 0x00;
    msg[pos++] = (uint8_t)logical_len;
    msg[pos++] = 0x00;
    memcpy(&msg[pos], packed, packed_len);
    pos += packed_len;
    msg[pos++] = 0xF7;

    ESP_LOGI(TAG,
             "Set block type -> block=%u type=%u params=%u",
             (unsigned)block, type_id, count);

    return nanocore_midi_send_sysex(msg, pos);
}

static esp_err_t send_block_param(uint8_t block,
                                  uint8_t param_index,
                                  float normalized)
{
    if ((block >= BLOCK_COUNT && block != WIRE_SYNTH_BLOCK) || param_index >= NC_PROTO_MAX_PARAMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    uint8_t logical[7] = {
        0x01,
        (uint8_t)block,
        param_index,
        0, 0, 0, 0
    };
    memcpy(&logical[3], &normalized, sizeof(normalized));

    uint8_t packed[8] = {0};
    size_t packed_len = pack7(logical, sizeof(logical),
                              packed, sizeof(packed));
    if (!packed_len) return ESP_FAIL;

    uint8_t msg[32] = {0};
    size_t pos = 0;
    msg[pos++] = 0xF0;
    msg[pos++] = NC_MFR;
    msg[pos++] = NC_TAG_N;
    msg[pos++] = NC_TAG_C;
    msg[pos++] = NC_DIR_HOST;
    msg[pos++] = 0x00;
    msg[pos++] = 0x02;
    msg[pos++] = next_seq();
    msg[pos++] = 0x00;
    msg[pos++] = OP_SET_FIELD;
    msg[pos++] = 0x00;
    msg[pos++] = 0x07;
    msg[pos++] = 0x00;
    memcpy(&msg[pos], packed, packed_len);
    pos += packed_len;
    msg[pos++] = 0xF7;

    ESP_LOGI(TAG,
             "Set block param -> block=%u param=%u norm=%.6f",
             (unsigned)block, param_index, (double)normalized);

    return nanocore_midi_send_sysex(msg, pos);
}

static esp_err_t send_rename(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;

    uint8_t chars[8] = {0};
    size_t n = strnlen(name, 8);
    for (size_t i = 0; i < n; ++i) {
        chars[i] = ((uint8_t)name[i]) & 0x7F;
    }

    /* Logical value = [nameLength, ASCII...].
     * ToneCommand inserts one zero separator after the first 6 logical bytes
     * when a seventh logical byte exists.
     */
    uint8_t logical[9] = {0};
    logical[0] = (uint8_t)n;
    memcpy(&logical[1], chars, n);
    size_t logical_len = n + 1;

    uint8_t value[10] = {0};
    size_t value_len = 0;
    if (logical_len <= 6) {
        memcpy(value, logical, logical_len);
        value_len = logical_len;
    } else {
        memcpy(value, logical, 6);
        value[6] = 0x00;
        memcpy(&value[7], &logical[6], logical_len - 6);
        value_len = logical_len + 1;
    }

    uint8_t msg[32] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C,
        NC_DIR_HOST, 0x00, 0x02, next_seq(), 0x00,
        OP_SET_FIELD, 0x00,
        (uint8_t)(1 + logical_len), /* field id + logical bytes */
        0x00, 0x00, FIELD_PRESET_NAME
    };

    size_t pos = 15;
    memcpy(&msg[pos], value, value_len);
    pos += value_len;
    msg[pos++] = 0xF7;

    ESP_LOGI(TAG, "Rename active preset -> \"%.*s\"",
             (int)n, name);
    return nanocore_midi_send_sysex(msg, pos);
}

static esp_err_t send_save(uint8_t slot)
{
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C,
        NC_DIR_HOST, 0x00, 0x02, next_seq(), 0x00,
        OP_SAVE_PRESET, 0x00, 0x01, 0x00, 0x00,
        slot, 0xF7
    };
    return nanocore_midi_send_sysex(msg, sizeof(msg));
}

static esp_err_t send_read_preset_window(uint8_t slot,
                                             uint16_t offset,
                                             uint8_t length)
{
    /*
     * ToneCommand window read:
     * [02, seqLo, seqHi, 41,00, len=4,00,
     *  slot, offsetLo, offsetHi, length]
     *
     * BLE replies carry at most 59 preset-data bytes per request.
     */
    uint8_t raw[] = {
        0x02, next_seq(), 0x00,
        OP_READ_PRESET, 0x00,
        0x04, 0x00,
        slot,
        (uint8_t)(offset & 0xFF),
        (uint8_t)((offset >> 8) & 0xFF),
        length
    };
    uint8_t packed[20] = {0};
    size_t packed_len = pack7(raw, sizeof(raw), packed, sizeof(packed));
    if (!packed_len) return ESP_FAIL;

    uint8_t msg[32] = {0};
    size_t p = 0;
    msg[p++] = 0xF0;
    msg[p++] = NC_MFR;
    msg[p++] = NC_TAG_N;
    msg[p++] = NC_TAG_C;
    msg[p++] = NC_DIR_HOST;
    memcpy(&msg[p], packed, packed_len);
    p += packed_len;
    msg[p++] = 0xF7;

    ESP_LOGI(TAG, "Read preset window slot=%u offset=%u len=%u",
             slot, offset, length);
    return nanocore_midi_send_sysex(msg, p);
}

static esp_err_t send_read_name_window(uint8_t slot)
{
    return send_read_preset_window(slot, 0, 59);
}

static void parse_name_payload(const uint8_t *payload, size_t len)
{
    if (!payload || len < 8) return;

    const uint8_t slot = payload[0];
    size_t anchor_pos = SIZE_MAX;

    /*
     * Real 0x41 captures show the name tag as:
     *
     *   02 <record-length> 00 02 10 <ASCII name...>
     *
     * The second byte is NOT fixed:
     *   factory/shorter names were observed with 0x07,
     *   the user-written 8-char "ColdCln1" uses 0x08.
     *
     * Therefore match the structural bytes and deliberately ignore the
     * variable length byte instead of hard-coding 0x07.
     */
    for (size_t i = 0; i + 5 <= len; ++i) {
        if (payload[i]     == 0x02 &&
            payload[i + 2] == 0x00 &&
            payload[i + 3] == 0x02 &&
            payload[i + 4] == 0x10) {
            anchor_pos = i + 5;
            break;
        }
    }

    if (anchor_pos == SIZE_MAX) {
        ESP_LOGW(TAG,
                 "Preset name tag not found in 0x41 payload len=%u",
                 (unsigned)len);
        return;
    }

    char name[13] = {0};
    size_t out = 0;

    while (out < 12 && anchor_pos + out < len) {
        const uint8_t ch = payload[anchor_pos + out];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) {
            ESP_LOGW(TAG,
                     "Preset name invalid byte 0x%02X at index=%u",
                     ch, (unsigned)out);
            return;
        }
        name[out++] = (char)ch;
    }

    if (!out) return;

    nanocore_protocol_event_t *e =
        scratch_event(NC_PROTO_EVENT_PRESET_NAME);
    e->slot = slot;
    memcpy(e->name, name, sizeof(e->name));
    queue_event(e);

    ESP_LOGI(TAG, "Preset %u name RX: %s", slot, name);
}


static bool parse_preset_window_header(const uint8_t *payload,
                                       size_t len,
                                       uint8_t *slot,
                                       uint16_t *offset,
                                       const uint8_t **data,
                                       size_t *data_len)
{
    if (!payload || len < 5) return false;

    if (slot) *slot = payload[0];
    if (offset) {
        *offset = (uint16_t)(payload[1] |
                            ((uint16_t)payload[2] << 8));
    }
    if (data) *data = &payload[5];
    if (data_len) *data_len = len - 5;
    return true;
}

static void parse_models_payload(const uint8_t *payload, size_t len)
{
    uint8_t slot = 0;
    uint16_t offset = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;

    if (!parse_preset_window_header(payload, len,
                                    &slot, &offset,
                                    &data, &data_len)) {
        return;
    }

    /*
     * Saved-preset footer:
     *   ... <AMP_INDEX> 21 01 <CAB_INDEX> 24 01 ...
     *
     * Both indices are plain 0..29 model ids.
     */
    for (size_t i = data_len; i-- > 1;) {
        if (i + 4 >= data_len) continue;
        if (data[i] == 0x21 &&
            data[i + 1] == 0x01 &&
            data[i + 3] == 0x24 &&
            data[i + 4] == 0x01) {

            const uint8_t amp = data[i - 1];
            const uint8_t cab = data[i + 2];

            nanocore_protocol_event_t *e =
                scratch_event(NC_PROTO_EVENT_PRESET_MODELS);
            if (amp >= 30 || cab >= 30) continue;
            e->slot = slot;
            e->preset_window_offset = offset;
            e->preset_window_total = (uint16_t)(payload[3] | ((uint16_t)payload[4] << 8));
            e->amp_model_known = amp < 30;
            e->cab_model_known = cab < 30;
            e->amp_model = amp;
            e->cab_model = cab;
            queue_event(e);

            ESP_LOGI(TAG,
                     "Preset models slot=%u offset=%u AMP=%u CAB=%u",
                     slot, offset, amp, cab);
            return;
        }
    }

    ESP_LOGW(TAG,
             "AMP/CAB footer not found slot=%u offset=%u data=%u",
             slot, offset, (unsigned)data_len);
    nanocore_protocol_event_t *e=scratch_event(NC_PROTO_EVENT_PRESET_MODELS);
    e->slot=slot;
    e->preset_window_offset=offset;
    e->preset_window_total=(uint16_t)(payload[3] | ((uint16_t)payload[4]<<8));
    queue_event(e);
}

/* The user's Stage 08A captures show a separate mode byte immediately
 * before the voice record. Keep it independent from voice enable.
 * Require a complete normal walk, validated chain and the exact observed
 * terminal voice record; unknown layouts must not become an ON state. */
static bool decode_synth_tail(nanocore_protocol_event_t *e,
                              const uint8_t *p, size_t len, size_t pos)
{
    static const uint8_t counts[10] = {6,5,3,5,5,5,5,3,5,3};
    if (!e->chain_order_known || pos > len || len - pos < 18) return false;
    const size_t at = pos + 15;
    const uint8_t mode = p[at - 1], on = p[at];
    const uint8_t voice = p[at + 1], count = p[at + 2];
    if (mode > 1 || on > 1 || voice >= 10 ||
        count != counts[voice] || count > NC_PROTO_SYNTH_MAX_PARAMS ||
        len - at != 3u + 4u * count) return false;
    for (uint8_t i = 0; i < count; ++i) {
        float value = read_float32_le(&p[at + 3 + 4u * i]);
        if (!(value >= 0.0f && value <= 1.0f)) return false;
        e->synth_params[i] = value;
    }
    e->synth_mode_known = true;
    e->synth_mode = mode != 0;
    e->synth_known = true;
    e->synth_on = on != 0;
    e->synth_variant = voice;
    e->synth_param_count = count;
    return true;
}

static void parse_live_payload(const uint8_t *p, size_t len, uint16_t seq)
{
    if (!p || len < 6 || p[0] != 0x03) {
        ESP_LOGW(TAG, "0x63 live payload too short/unexpected (%u)",
                 (unsigned)len);
        return;
    }

    nanocore_protocol_event_t *e =
        scratch_event(NC_PROTO_EVENT_LIVE_STATE);
    e->slot = p[1];
    e->live_seq = seq;
    e->live_header_byte = p[2];
    if (s_level_diagnostics) ESP_LOGI(TAG,"LEVEL|LIVE_HEADER|seq=%u|slot=%u|raw=%u",seq,p[1],p[2]);

    size_t pos = 3;

    /*
     * Reassembled live-profile block stream:
     * FX1 [on][variant][count][count * float32LE], then
     * FX2, AMP, CAB, DELAY, MOD, REVERB, EQ in the same shape.
     *
     * variant is kept RAW. Current firmware does not make AMP/CAB's raw
     * variant byte a trustworthy live model id, and REV/EQ have historical
     * encoding quirks. Stage 06 captures before translating.
     */

    if (pos + 3 > len) return;
    uint8_t count = p[pos + 2];
    if (count > NC_PROTO_MAX_PARAMS ||
        pos + 3 + (size_t)count * 4 > len) {
        ESP_LOGW(TAG, "Invalid FX1 live param count=%u len=%u",
                 count, (unsigned)len);
        return;
    }

    e->block_known[BLOCK_FX1] = true;
    e->block_on[BLOCK_FX1] = p[pos] != 0;
    e->block_profile_known[BLOCK_FX1] = true;
    e->block_variant[BLOCK_FX1] = p[pos + 1];
    e->block_param_count[BLOCK_FX1] = count;
    pos += 3;

    for (uint8_t i = 0; i < count; ++i, pos += 4) {
        e->block_params[BLOCK_FX1][i] = read_float32_le(&p[pos]);
    }

    for (int block = BLOCK_FX2; block < BLOCK_COUNT; ++block) {
        if (pos + 3 > len) {
            ESP_LOGW(TAG, "Live profile ended before block=%d", block);
            break;
        }

        const uint8_t on = p[pos];
        const uint8_t variant = p[pos + 1];
        count = p[pos + 2];
        pos += 3;

        /*
         * Firmware 1.2 adds Ping Pong to every DELAY type. Duck now reports
         * 9 live parameters, so all normal blocks must accept the full
         * NC_PROTO_MAX_PARAMS capacity instead of the old hard limit of 8.
         */
        const uint8_t max_count = NC_PROTO_MAX_PARAMS;

        if (count > max_count ||
            pos + (size_t)count * 4 > len) {
            ESP_LOGW(TAG,
                     "Invalid live block=%d count=%u pos=%u len=%u",
                     block, count, (unsigned)pos, (unsigned)len);
            break;
        }

        e->block_known[block] = true;
        e->block_on[block] = on != 0;
        e->block_profile_known[block] = true;
        e->block_variant[block] = variant;
        e->block_param_count[block] = count;

        for (uint8_t i = 0; i < count; ++i, pos += 4) {
            e->block_params[block][i] = read_float32_le(&p[pos]);
        }
    }

    /* Optional chain footer: only after all eight blocks were parsed. */
    bool complete = true;
    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!e->block_profile_known[b]) complete = false;
    }
    if (complete && pos + 9 <= len && p[pos] == 0x08) {
        bool seen[BLOCK_COUNT] = {0};
        bool valid = true;

        for (int i = 0; i < BLOCK_COUNT; ++i) {
            uint8_t id = p[pos + 1 + i];
            if (id >= BLOCK_COUNT || seen[id]) {
                valid = false;
                break;
            }
            seen[id] = true;
            e->chain_order[i] = id;
        }

        e->chain_order_known = valid;
    }

    if (decode_synth_tail(e, p, len, pos)) {
        ESP_LOGI(TAG, "SYNTH|LIVE|mode=%u|voice_on=%u|voice=%u|count=%u",
                 e->synth_mode, e->synth_on, e->synth_variant, e->synth_param_count);
    } else {
        ESP_LOGI(TAG, "SYNTH|LIVE|mode=UNKNOWN");
    }

    queue_event(e);

    ESP_LOGI(TAG,
             "LIVE slot=%u FX1=%u FX2=%u AMP=%u CAB=%u DEL=%u MOD=%u REV=%u EQ=%u",
             e->slot,
             e->block_on[BLOCK_FX1], e->block_on[BLOCK_FX2],
             e->block_on[BLOCK_AMP], e->block_on[BLOCK_CAB],
             e->block_on[BLOCK_DELAY], e->block_on[BLOCK_MOD],
             e->block_on[BLOCK_REVERB], e->block_on[BLOCK_EQ]);

    ESP_LOGD(TAG,
             "LIVE profile variants/counts: "
             "FX1=%u/%u FX2=%u/%u AMP=%u/%u CAB=%u/%u "
             "DEL=%u/%u MOD=%u/%u REV=%u/%u EQ=%u/%u SYN=%u/%u/%u",
             e->block_variant[BLOCK_FX1], e->block_param_count[BLOCK_FX1],
             e->block_variant[BLOCK_FX2], e->block_param_count[BLOCK_FX2],
             e->block_variant[BLOCK_AMP], e->block_param_count[BLOCK_AMP],
             e->block_variant[BLOCK_CAB], e->block_param_count[BLOCK_CAB],
             e->block_variant[BLOCK_DELAY], e->block_param_count[BLOCK_DELAY],
             e->block_variant[BLOCK_MOD], e->block_param_count[BLOCK_MOD],
             e->block_variant[BLOCK_REVERB], e->block_param_count[BLOCK_REVERB],
             e->block_variant[BLOCK_EQ], e->block_param_count[BLOCK_EQ],
             e->synth_known ? 1 : 0,
             e->synth_variant,
             e->synth_param_count);
}

/* Candidate inspection only: never updates normal block state or guesses
 * that Synth is enabled. Preserve the whole payload even if this walk fails. */
static void log_synth_capture(uint16_t seq, const uint8_t *p, size_t len)
{
    ESP_LOGI(TAG, "SYNMAP|BEGIN|seq=%u|len=%u|slot=%u",
             seq, (unsigned)len, len > 1 ? p[1] : 255);
    for (size_t off = 0; off < len; off += 16) {
        char line[16 * 3 + 1] = {0};
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; ++i) {
            snprintf(&line[i * 3], sizeof(line) - i * 3,
                     "%02X ", p[off + i]);
        }
        ESP_LOGI(TAG, "SYNMAP|HEX|%04u|%s", (unsigned)off, line);
    }

    static const char *const names[] = {
        "FX1", "FX2", "AMP", "CAB", "DELAY", "MOD", "REV", "EQ"
    };
    size_t pos = 3;
    bool complete = len >= 3 && p[0] == 0x03;
    for (unsigned b = 0; complete && b < 8; ++b) {
        if (pos + 3 > len || p[pos + 2] > NC_PROTO_MAX_PARAMS ||
            pos + 3 + 4u * p[pos + 2] > len) {
            complete = false;
            break;
        }
        uint8_t count = p[pos + 2];
        ESP_LOGI(TAG, "SYNMAP|BLOCK|%s|off=%u|on=%u|raw=%u|count=%u",
                 names[b], (unsigned)pos, p[pos], p[pos + 1], count);
        pos += 3;
        for (uint8_t i = 0; i < count; ++i, pos += 4) {
            ESP_LOGI(TAG, "SYNMAP|PARAM|%s|p%u=%.6f",
                     names[b], i, (double)read_float32_le(&p[pos]));
        }
    }

    if (complete) {
        ESP_LOGI(TAG, "SYNMAP|TAIL|off=%u|remaining=%u",
                 (unsigned)pos, (unsigned)(len - pos));
        /* External captures suggest a header 15 bytes after EQ. Only log
         * the candidate if the preceding 8-id chain footer is a permutation
         * and the voice/count pair is plausible. This is not a mode flag. */
        bool chain = pos + 9 <= len && p[pos] == 8;
        unsigned seen = 0;
        if (chain) {
            for (size_t i = 1; i <= 8; ++i) {
                uint8_t id = p[pos + i];
                if (id >= 8 || (seen & (1u << id))) {
                    chain = false;
                    break;
                }
                seen |= 1u << id;
            }
        }
        size_t at = pos + 15;
        static const uint8_t counts[10] = {6,5,3,5,5,5,5,3,5,3};
        if (chain && at + 3 <= len && p[at] <= 1 &&
            p[at + 1] < 10 && p[at + 2] == counts[p[at + 1]] &&
            at + 3 + 4u * p[at + 2] <= len) {
            uint8_t count = p[at + 2];
            ESP_LOGI(TAG, "SYNMAP|CANDIDATE|off=%u|on=%u|voice=%u|count=%u",
                     (unsigned)at, p[at], p[at + 1], count);
            ESP_LOGI(TAG, "SYNMAP|MODE_BYTE|off=%u|value=%u",
                     (unsigned)(at - 1), p[at - 1]);
            for (uint8_t i = 0; i < count; ++i) {
                ESP_LOGI(TAG, "SYNMAP|VOICE_PARAM|p%u=%.6f", i,
                         (double)read_float32_le(&p[at + 3 + 4u * i]));
            }
        } else {
            ESP_LOGI(TAG, "SYNMAP|CANDIDATE|not_matched");
        }
    } else {
        ESP_LOGW(TAG, "SYNMAP|BLOCK_WALK_INCOMPLETE|off=%u", (unsigned)pos);
    }
    ESP_LOGI(TAG, "SYNMAP|END|seq=%u", seq);
}

/* Called only by app_main after receiving the sequence-matched event. */
bool nanocore_protocol_print_synth_capture(uint16_t seq)
{
    size_t len = 0;
    bool ready = false;
    portENTER_CRITICAL(&s_capture_lock);
    if (s_capture_mailbox.ready && s_capture_mailbox.seq == seq) {
        len = s_capture_mailbox.len;
        memcpy(s_capture_print_buffer, s_capture_mailbox.data, len);
        s_capture_mailbox.ready = false;
        ready = true;
    }
    portEXIT_CRITICAL(&s_capture_lock);
    if (ready) log_synth_capture(seq, s_capture_print_buffer, len);
    return ready;
}

static void queue_synth_capture(uint16_t seq, const uint8_t *payload, size_t len)
{
    if (len > REASSEMBLY_MAX) return;
    portENTER_CRITICAL(&s_capture_lock);
    memcpy(s_capture_mailbox.data, payload, len);
    s_capture_mailbox.len = len;
    s_capture_mailbox.seq = seq;
    s_capture_mailbox.ready = true;
    portEXIT_CRITICAL(&s_capture_lock);
    nanocore_protocol_event_t *e =
        scratch_event(NC_PROTO_EVENT_SYNTH_CAPTURE);
    e->catalog_seq = seq;
    queue_event(e);
}

static void dispatch_complete_reply(uint16_t opcode, uint16_t seq,
                                    const uint8_t *payload,
                                    size_t len)
{
    if (opcode == OP_LIVE_STATE) {
        portENTER_CRITICAL(&s_seq_lock);
        bool capture = s_synth_capture_seq != 0 && s_synth_capture_seq == seq;
        if (capture) s_synth_capture_seq = 0;
        portEXIT_CRITICAL(&s_seq_lock);
        if (capture) {
            queue_synth_capture(seq, payload, len);
            return;
        }
        parse_live_payload(payload, len, seq);
    } else if (opcode == OP_READ_PRESET) {
        /*
         * 0x41 replies echo slot + offset at payload[0..2].
         * Offset 0 is our short name read; offset 200 is the saved-preset
         * tail containing authoritative AMP/CAB model indices.
         */
        uint16_t offset = len >= 3
            ? (uint16_t)(payload[1] | ((uint16_t)payload[2] << 8))
            : 0xFFFF;

        if (offset == 0) {
            parse_name_payload(payload, len);
        } else {
            parse_models_payload(payload, len);
        }
    } else if (opcode == OP_SAVE_PRESET &&
               s_wait_save_slot != 0xFF &&
               len > 0 &&
               payload[len - 1] == s_wait_save_slot) {
        xSemaphoreGive(s_save_ack);
    }
}

/* USB captures document a normal status-0 response; BLE may deliver the
 * same record through the 0x10/0x11 segmented envelope. Decode both without
 * touching the normal live/preset reassembly buffer. */
/* CAB is 30 bytes. AMP may have one extra trailing byte (31 bytes
 * confirmed on the user's firmware 1.2). The name offset and width
 * are identical; the unknown trailer is not part of the name. */
static bool catalog_record_length_valid(uint16_t opcode, size_t len)
{
    if (opcode == OP_CAB_CATALOG) return len == CATALOG_RECORD_LEN;
    if (opcode == OP_AMP_CATALOG) {
        return len == CATALOG_RECORD_LEN || len == CATALOG_AMP_RECORD_LEN;
    }
    return false;
}

static void emit_catalog_reply(uint16_t opcode, uint16_t seq, uint8_t status,
                               const uint8_t *payload, size_t len)
{
    if (status == 0) {
        if (!catalog_record_length_valid(opcode, len) ||
            payload[0] >= NC_PROTO_MODEL_COUNT ||
            payload[1] > 1) {
            ESP_LOGW(TAG, "Invalid catalog record op=0x%02X len=%u",
                     opcode, (unsigned)len);
            return;
        }
        if (payload[1] != 0) {
            for (size_t i = 0; i < NC_PROTO_MODEL_NAME_LEN; ++i) {
                uint8_t c = payload[CATALOG_NAME_OFFSET + i];
                if (c == 0) break;
                if (c < 0x20 || c > 0x7E) {
                    ESP_LOGW(TAG, "Invalid ASCII catalog name op=0x%02X slot=%u",
                             opcode, payload[0]);
                    return;
                }
            }
        }
    }

    nanocore_protocol_event_t *e =
        scratch_event(NC_PROTO_EVENT_MODEL_CATALOG);
    e->block = opcode == OP_AMP_CATALOG ? BLOCK_AMP : BLOCK_CAB;
    e->catalog_seq = seq;
    e->catalog_status = status;
    if (status == 0) {
        e->slot = payload[0];
        e->model_present = payload[1] != 0;
        if (e->model_present) {
            for (size_t i = 0; i < NC_PROTO_MODEL_NAME_LEN; ++i) {
                uint8_t c = payload[CATALOG_NAME_OFFSET + i];
                if (c == 0) break;
                e->model_name[i] = (char)c;
            }
        }
    }
    queue_event(e);
}

static void handle_catalog_reply(const uint8_t *msg, size_t len)
{
    bool amp = msg[9] == OP_AMP_CATALOG;
    if (amp) {
        ESP_LOGD(TAG, "CATALOG|AMP_RX|wire_len=%u", (unsigned)len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, len > 96 ? 96 : len, ESP_LOG_DEBUG);
    }
    uint8_t raw[96] = {0};
    size_t raw_len = unpack7(&msg[5], len - 6, raw, sizeof(raw));
    if (raw_len < 8 || raw[0] != 0x02) return;

    uint16_t seq = (uint16_t)(raw[1] | ((uint16_t)raw[2] << 8));
    uint16_t opcode = (uint16_t)(raw[3] | ((uint16_t)raw[4] << 8));
    uint8_t status = raw[5];
    uint16_t body_len = (uint16_t)(raw[6] | ((uint16_t)raw[7] << 8));
    if (amp) {
        ESP_LOGD(TAG, "CATALOG|AMP_DECODE|op=0x%02X|seq=%u|status=0x%02X|body=%u|raw=%u",
                 opcode, seq, status, body_len, (unsigned)raw_len);
    }
    if ((opcode != OP_AMP_CATALOG && opcode != OP_CAB_CATALOG) ||
        raw_len != (size_t)8 + body_len) {
        if (amp) ESP_LOGW(TAG, "CATALOG|AMP_BAD_HEADER");
        return;
    }

    const uint8_t *body = &raw[8];
    if (status != SEG_MORE && status != SEG_LAST) {
        s_catalog_reply.active = false;
        emit_catalog_reply(opcode, seq, status, body, body_len);
        return;
    }

    if (body_len < 4) return;
    uint16_t total = (uint16_t)(body[0] | ((uint16_t)body[1] << 8));
    uint16_t offset = (uint16_t)(body[2] | ((uint16_t)body[3] << 8));
    size_t data_len = body_len - 4;
    if (!catalog_record_length_valid(opcode, total) || data_len == 0 ||
        (size_t)offset + data_len > total) {
        ESP_LOGW(TAG, "CATALOG|BAD_SEGMENT_LENGTH|op=0x%02X|total=%u|off=%u|n=%u",
                 opcode, total, offset, (unsigned)data_len);
        s_catalog_reply.active = false;
        return;
    }

    if (offset == 0) {
        s_catalog_reply.active = true;
        s_catalog_reply.seq = seq;
        s_catalog_reply.opcode = opcode;
        s_catalog_reply.total = total;
        s_catalog_reply.received = 0;
    }
    if (!s_catalog_reply.active || s_catalog_reply.seq != seq ||
        s_catalog_reply.opcode != opcode ||
        s_catalog_reply.total != total ||
        s_catalog_reply.received != offset) {
        if (amp) ESP_LOGW(TAG, "CATALOG|AMP_BAD_SEGMENT|off=%u|received=%u",
                          offset, s_catalog_reply.received);
        return;
    }

    memcpy(&s_catalog_reply.data[offset], &body[4], data_len);
    s_catalog_reply.received += (uint16_t)data_len;
    if (status == SEG_LAST) {
        if (s_catalog_reply.received == s_catalog_reply.total) {
            emit_catalog_reply(opcode, seq, 0, s_catalog_reply.data,
                               s_catalog_reply.total);
        } else {
            ESP_LOGW(TAG, "CATALOG|INCOMPLETE|op=0x%02X|received=%u|total=%u",
                     opcode, s_catalog_reply.received, s_catalog_reply.total);
        }
        s_catalog_reply.active = false;
    }
}

static void handle_segmented_reply(const uint8_t *msg, size_t len)
{
    if (len < 14) return;

    uint8_t raw[96] = {0};
    size_t raw_len = unpack7(&msg[5], len - 6, raw, sizeof(raw));
    if (raw_len < 12 || raw[0] != 0x02) return;

    uint16_t seq = (uint16_t)(raw[1] | (raw[2] << 8));
    uint16_t opcode = (uint16_t)(raw[3] | (raw[4] << 8));
    uint8_t status = raw[5];

    if (status != SEG_MORE && status != SEG_LAST) return;

    uint16_t body_len = (uint16_t)(raw[6] | (raw[7] << 8));
    if (raw_len < (size_t)8 + body_len || body_len < 4) return;

    const uint8_t *body = &raw[8];
    uint16_t total = (uint16_t)(body[0] | (body[1] << 8));
    uint16_t offset = (uint16_t)(body[2] | (body[3] << 8));
    size_t data_len = body_len - 4;

    if (!total || total > REASSEMBLY_MAX ||
        offset + data_len > total) {
        ESP_LOGW(TAG, "Bad segmented reply op=0x%02X total=%u off=%u n=%u",
                 opcode, total, offset, (unsigned)data_len);
        s_reassembly.active = false;
        return;
    }

    if (!s_reassembly.active ||
        s_reassembly.seq != seq ||
        s_reassembly.opcode != opcode ||
        s_reassembly.total != total) {
        memset(&s_reassembly, 0, sizeof(s_reassembly));
        s_reassembly.active = true;
        s_reassembly.seq = seq;
        s_reassembly.opcode = opcode;
        s_reassembly.total = total;
    }

    memcpy(&s_reassembly.data[offset], &body[4], data_len);
    if (offset == s_reassembly.received) {
        s_reassembly.received += (uint16_t)data_len;
    }

    if (status == SEG_LAST) {
        if (s_reassembly.received == s_reassembly.total) {
            ESP_LOGI(TAG, "SysEx reply reassembled op=0x%02X bytes=%u",
                     opcode, s_reassembly.total);
            dispatch_complete_reply(opcode, seq,
                                    s_reassembly.data,
                                    s_reassembly.total);
        } else {
            ESP_LOGW(TAG, "Segmented reply incomplete op=0x%02X %u/%u",
                     opcode, s_reassembly.received, s_reassembly.total);
        }
        s_reassembly.active = false;
    }
}

/* 0x65/66: separate short reply path, never disturb 0x63 reassembly.
 * Documented USB response logical payload: 01 wireless loopback
 * signed7 input dB USB-volume BT-volume MIDI-channel. BLE single final
 * segment adds total/offset. Reject unrecognized layouts. */
static void emit_global(uint16_t seq,bool push,uint8_t gain,uint8_t usb,uint8_t bt)
{
    if(gain>127 || usb>100 || bt>100) return;
    nanocore_protocol_event_t *e=scratch_event(NC_PROTO_EVENT_GLOBAL_LEVELS);
    e->global_seq=seq;
    e->global_push=push;
    e->input_gain_db=(int8_t)(gain>=64 ? (int)gain-128 : gain);
    e->usb_volume=usb;
    e->bt_volume=bt;
    queue_event(e);
    ESP_LOGI(TAG,"LEVEL|GLOBAL|seq=%u|push=%u|input_db=%d|usb=%u|bt=%u",
             seq,push,(int)e->input_gain_db,usb,bt);
}

static void parse_global_reply(const uint8_t *msg,size_t len)
{
    uint8_t raw[96]={0};
    size_t n=unpack7(&msg[5],len-6,raw,sizeof(raw));
    if(n<8 || raw[0]!=2) return;
    uint16_t seq=(uint16_t)(raw[1]|((uint16_t)raw[2]<<8));
    size_t count=(size_t)(raw[6]|((uint16_t)raw[7]<<8));
    if(n!=8+count) return;
    const uint8_t *body=&raw[8];
    if(s_level_diagnostics) {
        ESP_LOGI(TAG,"LEVEL|GLOBAL_RAW|op=0x%02X|status=%u|body=%u",msg[9],raw[5],(unsigned)count);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG,raw,n,ESP_LOG_INFO);
    }
    if(raw[5]==SEG_LAST) {
        if(count!=11 || body[0]!=7 || body[1]!=0 || body[2]!=0 || body[3]!=0) return;
        body+=4;
        count-=4;
    } else if(raw[5]!=0) return;
    if(count!=7 || body[0]!=1 || body[1]>1 || body[2]>1 || body[6]>16) return;
    emit_global(seq,false,body[3],body[4],body[5]);
}

void nanocore_protocol_set_level_diagnostics(bool enabled)
{
    s_level_diagnostics=enabled;
}

static void parse_push(const uint8_t *msg, size_t len)
{
    if (len < 13) return;

    const uint8_t opcode = msg[9];

    if(s_level_diagnostics) {
        ESP_LOGI(TAG,"LEVEL|PUSH|op=0x%02X|len=%u",opcode,(unsigned)len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG,msg,len>48?48:len,ESP_LOG_INFO);
    }
    if (opcode == OP_GLOBAL_PUSH && len == 20 && msg[11] == 1) {
        emit_global(0,true,msg[15],msg[16],msg[17]);
    } else if (opcode == OP_ACTIVE_SLOT && len == 13) {
        /* 0x01 is ambiguous: volume/other controls can share this shape.
         * Never interpret its value as an authoritative slot. */
        uint8_t raw_slot=msg[11]&0x7f;
        nanocore_protocol_event_t *e=scratch_event(NC_PROTO_EVENT_ACTIVE_SLOT);
        e->slot=raw_slot;
        queue_event(e);
        ESP_LOGI(TAG,"SLOT|HINT|raw=%u",raw_slot);
    } else if (opcode == OP_BLOCK_STATE && len > 12) {
        uint8_t block_id = msg[11];
        if (block_id < BLOCK_COUNT) {
            nanocore_protocol_event_t *e =
                scratch_event(NC_PROTO_EVENT_BLOCK_STATE);
            e->block = (controller_block_t)block_id;
            e->on = msg[12] != 0;
            queue_event(e);
            ESP_LOGI(TAG, "Block push %u -> %s",
                     block_id, e->on ? "ON" : "OFF");
        }
    } else if (opcode == OP_PARAM_VALUE && len >= 19) {
        /*
         * Physical-panel parameter push:
         * 06 00 <block> <paramIndex> <internalTag> <float32LE>
         * The internalTag is intentionally ignored.
         */
        uint8_t block_id = msg[11];
        uint8_t param_index = msg[12];

        if ((block_id < BLOCK_COUNT && param_index < NC_PROTO_MAX_PARAMS) ||
            (block_id == WIRE_SYNTH_BLOCK && param_index < NC_PROTO_SYNTH_MAX_PARAMS)) {
            float value = read_float32_le(&msg[14]);
            if (value >= 0.0f && value <= 1.0f) {
                nanocore_protocol_event_t *e =
                    scratch_event(block_id == WIRE_SYNTH_BLOCK ?
                        NC_PROTO_EVENT_SYNTH_PARAM_VALUE : NC_PROTO_EVENT_PARAM_VALUE);
                if (block_id < BLOCK_COUNT) e->block = (controller_block_t)block_id;
                e->param_index = param_index;
                e->param_value = value;
                queue_event(e);

                ESP_LOGD(TAG,
                         "Param push block=%u p%u=%.6f",
                         block_id, param_index, (double)value);
            }
        }
    }
}

static void ping_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_connected && nanocore_midi_is_ready()) {
            (void)send_short_command(OP_PING);
        }
        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
    }
}

esp_err_t nanocore_protocol_init(void)
{
    s_events = xQueueCreate(PROTO_EVENT_QUEUE,
                            sizeof(nanocore_protocol_event_t));
    s_save_ack = xSemaphoreCreateBinary();
    if (!s_events || !s_save_ack) return ESP_ERR_NO_MEM;

    if (xTaskCreate(ping_task, "nc_proto_ping", 3072, NULL, 4,
                    &s_ping_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ToneCommand-compatible SysEx protocol ready");
    return ESP_OK;
}

void nanocore_protocol_set_connected(bool connected)
{
    s_connected = connected;
    portENTER_CRITICAL(&s_seq_lock);
    s_synth_capture_seq = 0;
    portEXIT_CRITICAL(&s_seq_lock);
    portENTER_CRITICAL(&s_capture_lock);
    s_capture_mailbox.ready = false;
    portEXIT_CRITICAL(&s_capture_lock);
    memset(&s_reassembly, 0, sizeof(s_reassembly));
    memset(&s_catalog_reply, 0, sizeof(s_catalog_reply));

    if (connected && nanocore_midi_is_ready()) {
        (void)send_short_command(OP_PING);
    }
}

esp_err_t nanocore_protocol_request_live_state_seq(uint16_t *seq_out)
{
    if (!s_connected || !nanocore_midi_is_ready() || !seq_out)
        return ESP_ERR_INVALID_STATE;
    uint8_t seq = next_seq();
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C, NC_DIR_HOST,
        0x00, 0x02, seq, 0x00, OP_LIVE_STATE, 0x00, 0x00, 0x00, 0xF7
    };
    *seq_out = seq;
    return nanocore_midi_send_sysex(msg, sizeof(msg));
}

esp_err_t nanocore_protocol_request_live_state(void)
{
    uint16_t seq;
    return nanocore_protocol_request_live_state_seq(&seq);
}

esp_err_t nanocore_protocol_request_global_levels(uint16_t *seq_out)
{
    if(!seq_out || !s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    uint8_t seq=next_seq();
    uint8_t msg[]={0xF0,NC_MFR,NC_TAG_N,NC_TAG_C,NC_DIR_HOST,
        0,2,seq,0,OP_GLOBAL_READ,0,0,0,0xF7};
    *seq_out=seq;
    return nanocore_midi_send_sysex(msg,sizeof(msg));
}

esp_err_t nanocore_protocol_set_input_gain(int8_t db,uint16_t *seq_out)
{
    if(!seq_out || !s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    /* FW1.2 physical trace: requested signed7 -1 (127) became +20 dB.
     * Only the nonnegative range has been verified. Never encode negative. */
    if(db < 0 || db > 20) return ESP_ERR_INVALID_ARG;
    uint8_t seq=next_seq();
    uint8_t msg[]={0xF0,NC_MFR,NC_TAG_N,NC_TAG_C,NC_DIR_HOST,
        0,2,seq,0,OP_GLOBAL_SET,0,2,0,0,4,(uint8_t)db,0xF7};
    *seq_out=seq;
    ESP_LOGI(TAG,"LEVEL|INPUT_TX|db=%d|seq=%u",(int)db,seq);
    return nanocore_midi_send_sysex(msg,sizeof(msg));
}

esp_err_t nanocore_protocol_set_synth_mode(bool enabled)
{
    if (!s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "SYNTH|MODE_TX|value=%u", enabled);
    return send_scalar_field(FIELD_SYNTH_MODE, enabled ? 1 : 0);
}

esp_err_t nanocore_protocol_set_synth_voice(uint8_t voice, const float *params, uint8_t count)
{
    static const uint8_t counts[] = {6,5,3,5,5,5,5,3,5,3};
    if (!s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    if (voice >= sizeof(counts) || count != counts[voice] || !params)
        return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < count; ++i)
        if (!(params[i] >= 0.0f && params[i] <= 1.0f)) return ESP_ERR_INVALID_ARG;
    esp_err_t rc = send_type_prime();
    if (rc != ESP_OK) return rc;
    vTaskDelay(1);
    return send_block_type(WIRE_SYNTH_BLOCK, voice, params, count);
}

esp_err_t nanocore_protocol_set_synth_param(uint8_t index, float normalized)
{
    if (!s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    if (index >= NC_PROTO_SYNTH_MAX_PARAMS || !(normalized >= 0.0f && normalized <= 1.0f))
        return ESP_ERR_INVALID_ARG;
    return send_block_param(WIRE_SYNTH_BLOCK, index, normalized);
}

esp_err_t nanocore_protocol_request_synth_capture(uint16_t *seq_out)
{
    if (!seq_out) return ESP_ERR_INVALID_ARG;
    if (!s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;

    uint8_t seq = next_seq();
    uint8_t msg[] = {
        0xF0, NC_MFR, NC_TAG_N, NC_TAG_C, NC_DIR_HOST,
        0x00, 0x02, seq, 0x00, OP_LIVE_STATE, 0x00, 0x00, 0x00, 0xF7
    };
    portENTER_CRITICAL(&s_seq_lock);
    s_synth_capture_seq = seq;
    portEXIT_CRITICAL(&s_seq_lock);
    *seq_out = seq;
    esp_err_t rc = nanocore_midi_send_sysex(msg, sizeof(msg));
    if (rc != ESP_OK) {
        portENTER_CRITICAL(&s_seq_lock);
        if (s_synth_capture_seq == seq) s_synth_capture_seq = 0;
        portEXIT_CRITICAL(&s_seq_lock);
    }
    return rc;
}

esp_err_t nanocore_protocol_request_preset_name(uint8_t slot)
{
    if (!s_connected || slot >= CTRL_PRESET_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_read_name_window(slot);
}

esp_err_t nanocore_protocol_request_preset_models(uint8_t slot)
{
    if (!s_connected || slot >= CTRL_PRESET_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Real saved presets are ~265 bytes and the AMP/CAB footer lives near the
     * tail of page 2. A real captured preset places AMP at absolute byte 258
     * and the 0x21 footer marker at byte 259. Starting at 220 keeps the whole
     * footer inside one BLE-capped 59-byte response (220..278) instead of
     * splitting it exactly at the 59-byte boundary.
     */
    return send_read_preset_window(slot, 220, 59);
}

esp_err_t nanocore_protocol_request_preset_models_window(uint8_t slot, uint16_t offset)
{
    if (!s_connected || slot>=CTRL_PRESET_COUNT || offset==0 || offset>1024)
        return ESP_ERR_INVALID_ARG;
    return send_read_preset_window(slot,offset,59);
}

esp_err_t nanocore_protocol_request_cab_storage_info(void)
{
    if (!s_connected || !nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;
    return send_short_command(OP_CAB_STORAGE_INFO);
}

esp_err_t nanocore_protocol_request_amp_storage_info(void)
{
    if (!s_connected || !nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_short_command(OP_AMP_STORAGE_INFO);
}

esp_err_t nanocore_protocol_request_model_catalog(
    controller_block_t block, uint8_t model_id, uint16_t *seq_out)
{
    if (!s_connected || !nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!seq_out || model_id >= NC_PROTO_MODEL_COUNT ||
        (block != BLOCK_AMP && block != BLOCK_CAB)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t opcode = block == BLOCK_AMP ? OP_AMP_CATALOG : OP_CAB_CATALOG;
    uint8_t seq = next_seq();
    const uint8_t logical[] = {0x02, seq, 0, opcode, 0, 1, 0, model_id};
    uint8_t msg[24] = {0xF0, NC_MFR, NC_TAG_N, NC_TAG_C, NC_DIR_HOST};
    size_t packed = pack7(logical, sizeof(logical), &msg[5],
                          sizeof(msg) - 6);
    if (!packed) return ESP_FAIL;
    msg[5 + packed] = 0xF7;
    *seq_out = seq;
    if (block == BLOCK_AMP) {
        ESP_LOGD(TAG, "CATALOG|AMP_TX|slot=%u|seq=%u", model_id, seq);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, 6 + packed, ESP_LOG_DEBUG);
    }
    return nanocore_midi_send_sysex(msg, 6 + packed);
}

esp_err_t nanocore_protocol_set_block_type(
    controller_block_t block,
    uint8_t type_id,
    const float *default_normalized,
    uint8_t param_count)
{
    if (!s_connected || !nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = send_type_prime();
    if (rc != ESP_OK) return rc;

    /*
     * Keep a small gap between the prime and the type payload. On a 100 Hz
     * tick this becomes one scheduler tick, which is still negligible to UI.
     */
    TickType_t gap = pdMS_TO_TICKS(4);
    if (gap < 1) gap = 1;
    vTaskDelay(gap);

    return send_block_type(block, type_id,
                           default_normalized, param_count);
}

esp_err_t nanocore_protocol_set_block_param(
    controller_block_t block,
    uint8_t param_index,
    float normalized_value)
{
    if (!s_connected || !nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    return send_block_param(block, param_index, normalized_value);
}

esp_err_t nanocore_protocol_set_block_model(
    controller_block_t block,
    uint8_t model_id)
{
    if (!s_connected || !nanocore_midi_is_ready() || model_id >= 30) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t field;
    if (block == BLOCK_AMP) {
        field = FIELD_AMP_MODEL;
    } else if (block == BLOCK_CAB) {
        field = FIELD_CAB_MODEL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Set %s model -> %u",
             block == BLOCK_AMP ? "AMP" : "CAB", model_id);
    return send_scalar_field(field, model_id);
}

esp_err_t nanocore_protocol_rename_and_save(uint8_t slot,
                                           const char *name,
                                           TickType_t timeout)
{
    if (!s_connected || !nanocore_midi_is_ready() ||
        slot >= CTRL_PRESET_COUNT || !name) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = send_rename(name);
    if (rc != ESP_OK) return rc;
    vTaskDelay(pdMS_TO_TICKS(RENAME_SETTLE_MS));

    for (int attempt = 1; attempt <= SAVE_RETRIES; ++attempt) {
        while (xSemaphoreTake(s_save_ack, 0) == pdTRUE) {}
        s_wait_save_slot = slot;

        rc = send_save(slot);
        if (rc != ESP_OK) {
            s_wait_save_slot = 0xFF;
            return rc;
        }

        ESP_LOGI(TAG, "SAVE slot=%u attempt %d/%d",
                 slot, attempt, SAVE_RETRIES);

        if (xSemaphoreTake(s_save_ack, timeout) == pdTRUE) {
            s_wait_save_slot = 0xFF;
            ESP_LOGI(TAG, "SAVE slot=%u confirmed by NanoCore", slot);
            return ESP_OK;
        }
    }

    s_wait_save_slot = 0xFF;
    ESP_LOGE(TAG, "SAVE slot=%u: no ACK", slot);
    return ESP_ERR_TIMEOUT;
}

void nanocore_protocol_on_sysex(const uint8_t *msg, size_t len)
{
    if (!msg || len < 12 ||
        msg[0] != 0xF0 || msg[len - 1] != 0xF7 ||
        msg[1] != NC_MFR || msg[2] != NC_TAG_N || msg[3] != NC_TAG_C) {
        return;
    }

    ESP_LOGD(TAG, "RX SysEx dir=0x%02X op=0x%02X len=%u",
             msg[4], msg[9], (unsigned)len);

    if (msg[4] == NC_DIR_PUSH) {
        parse_push(msg, len);
        return;
    }

    if (msg[4] != NC_DIR_REPLY) return;

    // Heartbeat replies are intentionally not state-bearing. Ignoring them
    // also prevents a one-segment 0x68 reply from disturbing a larger 0x63
    // or 0x41 BLE reassembly in progress.
    if (msg[9] == OP_PING) return;
    if (msg[9] == OP_GLOBAL_READ || msg[9] == OP_GLOBAL_SET) {
        parse_global_reply(msg,len);
        return;
    }

    /* re-nanocore docs/protocol.txt identifies 0x30/0x50 as summary
     * reads, not proven bulk name pages. Keep them outside live reassembly.
     * 0x6D acknowledgements are currently not consumed by application;
     * do not let them replace a fragmented authoritative 0x63 reply. */
    if (msg[9] == OP_SET_FIELD) return;
    if (msg[9] == OP_AMP_STORAGE_INFO || msg[9] == OP_CAB_STORAGE_INFO) {
        uint8_t raw[96]={0};
        size_t n=unpack7(&msg[5],len-6,raw,sizeof(raw));
        if(n>=8 && raw[0]==2) {
            ESP_LOGI(TAG,"CATALOG|SUMMARY_RX|op=0x%02X|status=0x%02X|body=%u",
                     msg[9],raw[5],(unsigned)(raw[6]|((uint16_t)raw[7]<<8)));
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,raw,n,ESP_LOG_INFO);
        }
        return;
    }

    if (msg[9] == OP_AMP_CATALOG || msg[9] == OP_CAB_CATALOG) {
        handle_catalog_reply(msg, len);
        return;
    }

    /* Short save ACK uses the verified direct, non-packed shape. */
    if (msg[9] == OP_SAVE_PRESET &&
        s_wait_save_slot != 0xFF &&
        len > 15 &&
        msg[15] == s_wait_save_slot) {
        xSemaphoreGive(s_save_ack);
        return;
    }

    /*
     * Over BLE, larger replies use status 0x10/0x11 after 7-bit unpacking.
     * Small replies (ping/save) do not need reassembly.
     */
    handle_segmented_reply(msg, len);
}

bool nanocore_protocol_get_event(nanocore_protocol_event_t *event,
                                 TickType_t timeout)
{
    if (!event || !s_events) return false;
    return xQueueReceive(s_events, event, timeout) == pdTRUE;
}
