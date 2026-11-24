/*
 * BLE-MIDI 1.0 transmitter for NANOCORE.
 *
 * BLE-MIDI service:
 *   03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 * MIDI I/O characteristic:
 *   7772E5DB-3868-4112-A1A9-F2669D106BF3
 *
 * BLE-MIDI requires MIDI writes using GATT Write Without Response.
 */

#include "nanocore_midi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"

static const char *TAG = "NC_MIDI";

static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_midi_char_handle = 0;

static uint16_t midi_timestamp_ms(void)
{
    // BLE-MIDI uses a rolling 13-bit millisecond timestamp.
    return (uint16_t)((esp_timer_get_time() / 1000ULL) & 0x1FFF);
}

static esp_err_t send_ble_midi_message(const uint8_t *midi,
                                       uint8_t midi_len)
{
    if (!midi || midi_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    // 2 timestamp bytes + short MIDI message.
    uint8_t packet[5] = {0};
    if ((size_t)midi_len + 2 > sizeof(packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint16_t ts = midi_timestamp_ms();

    packet[0] = 0x80 | ((ts >> 7) & 0x3F);
    packet[1] = 0x80 | (ts & 0x7F);

    for (uint8_t i = 0; i < midi_len; ++i) {
        packet[i + 2] = midi[i];
    }

    // BLE-MIDI MIDI I/O requires Write Without Response.
    int rc = ble_gattc_write_no_rsp_flat(
        s_conn_handle,
        s_midi_char_handle,
        packet,
        (uint16_t)(midi_len + 2));

    if (rc != 0) {
        ESP_LOGE(TAG,
                 "BLE MIDI write failed rc=%d conn=%u handle=0x%04X",
                 rc,
                 s_conn_handle,
                 s_midi_char_handle);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "BLE MIDI TX:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG,
                             packet,
                             midi_len + 2,
                             ESP_LOG_DEBUG);

    return ESP_OK;
}

void nanocore_midi_set_link(uint16_t conn_handle,
                            uint16_t midi_char_handle)
{
    s_conn_handle = conn_handle;
    s_midi_char_handle = midi_char_handle;

    ESP_LOGI(TAG,
             "MIDI link READY conn=%u char=0x%04X",
             s_conn_handle,
             s_midi_char_handle);
}

void nanocore_midi_clear_link(void)
{
    s_conn_handle = 0xFFFF;
    s_midi_char_handle = 0;
}

bool nanocore_midi_is_ready(void)
{
    return s_conn_handle != 0xFFFF && s_midi_char_handle != 0;
}

esp_err_t nanocore_midi_send_program_change(uint8_t channel_1based,
                                            uint8_t program)
{
    if (channel_1based < 1 || channel_1based > 16 || program > 127) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t midi[2] = {
        (uint8_t)(0xC0 | ((channel_1based - 1) & 0x0F)),
        program
    };

    esp_err_t rc = send_ble_midi_message(midi, sizeof(midi));

    if (rc == ESP_OK) {
        ESP_LOGI(TAG,
                 "Program Change -> ch=%u program=%u",
                 channel_1based,
                 program);
    }

    return rc;
}

esp_err_t nanocore_midi_send_cc(uint8_t channel_1based,
                                uint8_t cc,
                                uint8_t value)
{
    if (channel_1based < 1 || channel_1based > 16 ||
        cc > 127 || value > 127) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t midi[3] = {
        (uint8_t)(0xB0 | ((channel_1based - 1) & 0x0F)),
        cc,
        value
    };

    return send_ble_midi_message(midi, sizeof(midi));
}


esp_err_t nanocore_midi_send_sysex(const uint8_t *sysex, size_t len)
{
    if (!sysex || len < 2 || sysex[0] != 0xF0 || sysex[len - 1] != 0xF7) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nanocore_midi_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Keep each ATT payload at 20 bytes even though we negotiate a larger MTU.
     * 2 bytes are BLE-MIDI header/timestamp, leaving 18 raw MIDI bytes.
     * A long SysEx continues in the next packet without a second F0.
     */
    const size_t max_midi = 18;
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > max_midi) chunk = max_midi;

        uint8_t packet[20] = {0};
        const uint16_t ts = midi_timestamp_ms();
        packet[0] = 0x80 | ((ts >> 7) & 0x3F);
        packet[1] = 0x80 | (ts & 0x7F);
        memcpy(&packet[2], &sysex[off], chunk);

        int rc = ble_gattc_write_no_rsp_flat(
            s_conn_handle,
            s_midi_char_handle,
            packet,
            (uint16_t)(chunk + 2));

        if (rc != 0) {
            ESP_LOGE(TAG, "BLE SysEx write failed rc=%d off=%u",
                     rc, (unsigned)off);
            return ESP_FAIL;
        }

        off += chunk;
        if (off < len) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    ESP_LOGD(TAG, "SysEx TX len=%u", (unsigned)len);
    return ESP_OK;
}
