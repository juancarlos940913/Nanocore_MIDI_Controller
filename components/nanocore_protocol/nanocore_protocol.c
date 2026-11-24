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
#define OP_READ_PRESET         0x41
#define OP_SAVE_PRESET         0x46
#define OP_LIVE_STATE          0x63
#define OP_PING                0x68
#define OP_SET_FIELD           0x6D
#define FIELD_PRESET_NAME      0x08

#define SEG_MORE               0x10
#define SEG_LAST               0x11

#define PROTO_EVENT_QUEUE      16
#define REASSEMBLY_MAX       2048
#define PING_INTERVAL_MS     3000
#define RENAME_SETTLE_MS       55
#define SAVE_RETRIES            3

static const char *TAG = "NC_PROTO";

static QueueHandle_t s_events;
static SemaphoreHandle_t s_save_ack;
static TaskHandle_t s_ping_task;

static volatile bool s_connected;
static uint8_t s_seq;
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

static uint8_t next_seq(void)
{
    s_seq = (uint8_t)((s_seq + 1) & 0x7F);
    if (s_seq == 0) s_seq = 1;
    return s_seq;
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

static esp_err_t send_read_name_window(uint8_t slot)
{
    /* Raw command body documented by ToneCommand captures:
     * [02, seqLo, seqHi, 41,00, len=4,00, slot, offsetLo,offsetHi, length]
     * Then 7-bit pack everything after F0 7D 4E 43 70.
     */
    uint8_t raw[] = {
        0x02, next_seq(), 0x00,
        OP_READ_PRESET, 0x00,
        0x04, 0x00,
        slot, 0x00, 0x00, 59
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

    return nanocore_midi_send_sysex(msg, p);
}

static void parse_name_payload(const uint8_t *payload, size_t len)
{
    static const uint8_t anchor[] = {0x02,0x07,0x00,0x02,0x10};

    if (!payload || len < sizeof(anchor) + 3) return;

    uint8_t slot = payload[0];
    size_t anchor_pos = SIZE_MAX;
    for (size_t i = 0; i + sizeof(anchor) <= len; ++i) {
        if (memcmp(&payload[i], anchor, sizeof(anchor)) == 0) {
            anchor_pos = i + sizeof(anchor);
            break;
        }
    }
    if (anchor_pos == SIZE_MAX) return;

    char name[13] = {0};
    size_t out = 0;
    for (size_t read = 0; out < 12 && anchor_pos + read < len; ++read) {
        if (read == 2 || read == 10) continue;
        uint8_t ch = payload[anchor_pos + read];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) return;
        name[out++] = (char)ch;
    }
    if (!out) return;

    nanocore_protocol_event_t e = {
        .type = NC_PROTO_EVENT_PRESET_NAME,
        .slot = slot,
    };
    memcpy(e.name, name, sizeof(e.name));
    queue_event(&e);

    ESP_LOGI(TAG, "Preset %u name RX: %s", slot, name);
}

static void parse_live_payload(const uint8_t *p, size_t len)
{
    if (!p || len < 6 || p[0] != 0x03) {
        ESP_LOGW(TAG, "0x63 live payload too short/unexpected (%u)",
                 (unsigned)len);
        return;
    }

    nanocore_protocol_event_t e = {
        .type = NC_PROTO_EVENT_LIVE_STATE,
        .slot = p[1],
    };

    size_t pos = 3;

    /* FX1: [on][type][count] + floats */
    if (pos + 3 > len) return;
    uint8_t count = p[pos + 2];
    if (count > 9 || pos + 3 + (size_t)count * 4 > len) return;
    e.block_known[BLOCK_FX1] = true;
    e.block_on[BLOCK_FX1] = p[pos] != 0;
    pos += 3 + (size_t)count * 4;

    /* The actual NanoCore serialization order is:
     * FX2, AMP, CAB, DELAY, MOD, REVERB, EQ.
     * Stage05.1's controller enum intentionally matches that order.
     */
    for (int block = BLOCK_FX2; block < BLOCK_COUNT; ++block) {
        if (pos + 3 > len) return;
        count = p[pos + 2];
        uint8_t max_count = block == BLOCK_EQ ? 9 : 8;
        if (count > max_count ||
            pos + 3 + (size_t)count * 4 > len) {
            return;
        }

        e.block_known[block] = true;
        e.block_on[block] = p[pos] != 0;
        pos += 3 + (size_t)count * 4;
    }

    queue_event(&e);

    ESP_LOGI(TAG,
             "LIVE slot=%u FX1=%u FX2=%u AMP=%u CAB=%u DEL=%u MOD=%u REV=%u EQ=%u",
             e.slot,
             e.block_on[BLOCK_FX1], e.block_on[BLOCK_FX2],
             e.block_on[BLOCK_AMP], e.block_on[BLOCK_CAB],
             e.block_on[BLOCK_DELAY], e.block_on[BLOCK_MOD],
             e.block_on[BLOCK_REVERB], e.block_on[BLOCK_EQ]);
}

static void dispatch_complete_reply(uint16_t opcode,
                                    const uint8_t *payload,
                                    size_t len)
{
    if (opcode == OP_LIVE_STATE) {
        parse_live_payload(payload, len);
    } else if (opcode == OP_READ_PRESET) {
        parse_name_payload(payload, len);
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
            dispatch_complete_reply(opcode,
                                    s_reassembly.data,
                                    s_reassembly.total);
        } else {
            ESP_LOGW(TAG, "Segmented reply incomplete op=0x%02X %u/%u",
                     opcode, s_reassembly.received, s_reassembly.total);
        }
        s_reassembly.active = false;
    }
}

static void parse_push(const uint8_t *msg, size_t len)
{
    if (len < 13) return;

    const uint8_t opcode = msg[9];

    if (opcode == OP_ACTIVE_SLOT && len > 11) {
        nanocore_protocol_event_t e = {
            .type = NC_PROTO_EVENT_ACTIVE_SLOT,
            .slot = msg[11] & 0x7F,
        };
        queue_event(&e);
        ESP_LOGI(TAG, "Active slot push -> %u", e.slot);
    } else if (opcode == OP_BLOCK_STATE && len > 12) {
        uint8_t block_id = msg[11];
        if (block_id < BLOCK_COUNT) {
            nanocore_protocol_event_t e = {
                .type = NC_PROTO_EVENT_BLOCK_STATE,
                .block = (controller_block_t)block_id,
                .on = msg[12] != 0,
            };
            queue_event(&e);
            ESP_LOGI(TAG, "Block push %u -> %s",
                     block_id, e.on ? "ON" : "OFF");
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
    memset(&s_reassembly, 0, sizeof(s_reassembly));

    if (connected && nanocore_midi_is_ready()) {
        (void)send_short_command(OP_PING);
    }
}

esp_err_t nanocore_protocol_request_live_state(void)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    return send_short_command(OP_LIVE_STATE);
}

esp_err_t nanocore_protocol_request_preset_name(uint8_t slot)
{
    if (!s_connected || slot >= CTRL_PRESET_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_read_name_window(slot);
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
