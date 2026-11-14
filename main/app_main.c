/*
 * Nanocore MIDI Controller
 * Stage 04 - Landscape UI + PRESET/STOMP/SCENE modes
 *
 * Existing PCB test controls:
 *
 *   Hold BTN_PROGRAM + BTN_STOP ~700 ms:
 *       PRESET -> STOMP -> SCENE -> PRESET
 *
 *   BTN_FUNCTION short:
 *       same mode cycle (development shortcut)
 *
 *   PRESET:
 *       BTN_PROGRAM = previous preset
 *       BTN_STOP    = next preset
 *
 *   STOMP:
 *       BTN_PROGRAM = FX2 toggle (CC21)
 *       BTN_STOP    = Delay toggle (CC26)
 *
 *   SCENE:
 *       BTN_PROGRAM = previous demo scene
 *       BTN_STOP    = next demo scene
 *
 * Display:
 *   ST7789V native 240x320 rotated to 320x240 landscape.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_conn_mgr.h"
#include "esp_ble_midi.h"
#include "esp_ble_midi_svc.h"

#include "board_pins.h"
#include "display_ui.h"
#include "nanocore_midi.h"
#include "snapshot_manager.h"

static const char *TAG = "NC_STAGE04";

static const uint8_t MIDI_SERVICE_UUID[16] = BLE_MIDI_SERVICE_UUID128;
static const uint8_t MIDI_CHAR_UUID[16]    = BLE_MIDI_CHAR_UUID128;

#define NANOCORE_MIDI_CHANNEL       1
#define NANOCORE_PRESET_MIN         0
#define NANOCORE_PRESET_MAX        63

#define CC_FX1_ONOFF               20
#define CC_FX2_ONOFF               21
#define CC_AMP_ONOFF               23
#define CC_CAB_ONOFF               24
#define CC_MOD_ONOFF               25
#define CC_DELAY_ONOFF             26
#define CC_REVERB_ONOFF            27
#define CC_EQ_ONOFF                28
#define CC_AMP_TYPE                43

#define BUTTON_POLL_MS             10
#define BUTTON_DEBOUNCE_MS         35
#define MODE_CHORD_HOLD_MS        700

// Stage 04.1 scene transport tuning.
// Previous 4 ms spacing was too aggressive for the real NANOCORE.
// Absolute CC values are safe to repeat, so each scene action is sent
// twice with a conservative gap.
#define SCENE_RETRY_GAP_MS         25
#define SCENE_ACTION_GAP_MS        45
#define SCENE_PHASE_GAP_MS         80
#define SCENE_FEEDBACK_WAIT_MS    140
#define AMP_AUDITION_GAP_MS        45

static bool s_target_found = false;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint16_t s_midi_char_handle = 0;

static display_ui_state_t s_ui = {
    .ble_connected = false,
    .midi_ready = false,
    .mode = UI_MODE_PRESET,
    .preset = 0,
    .scene = 0,
    .fx1 = UI_BLOCK_UNKNOWN,
    .fx2 = UI_BLOCK_UNKNOWN,
    .amp = UI_BLOCK_UNKNOWN,
    .cab = UI_BLOCK_UNKNOWN,
    .mod = UI_BLOCK_UNKNOWN,
    .delay = UI_BLOCK_UNKNOWN,
    .reverb = UI_BLOCK_UNKNOWN,
    .eq = UI_BLOCK_UNKNOWN,
};

static void render_ui(void)
{
    if (display_ui_is_ready()) {
        display_ui_render(&s_ui);
    }
}

static void set_blocks_unknown(void)
{
    s_ui.fx1 = UI_BLOCK_UNKNOWN;
    s_ui.fx2 = UI_BLOCK_UNKNOWN;
    s_ui.amp = UI_BLOCK_UNKNOWN;
    s_ui.cab = UI_BLOCK_UNKNOWN;
    s_ui.mod = UI_BLOCK_UNKNOWN;
    s_ui.delay = UI_BLOCK_UNKNOWN;
    s_ui.reverb = UI_BLOCK_UNKNOWN;
    s_ui.eq = UI_BLOCK_UNKNOWN;
}

static void refresh_scene_amp_ui(void)
{
    const snapshot_t *snap = snapshot_manager_get(s_ui.scene);

    s_ui.scene_amp_configured = snap && snap->amp_configured;
    s_ui.scene_amp_type = (snap && snap->amp_configured)
                            ? snap->amp_type_id : 0;

    if (!s_ui.amp_editing) {
        s_ui.amp_edit_candidate = s_ui.scene_amp_type;
    }
}

static esp_err_t send_amp_type(uint8_t amp_type_id)
{
    if (!nanocore_midi_is_ready()) {
        ESP_LOGW(TAG, "MIDI not ready; Amp Type %u not sent",
                 amp_type_id);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = nanocore_midi_send_cc(
        NANOCORE_MIDI_CHANNEL,
        CC_AMP_TYPE,
        amp_type_id & 0x7F);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "AMP TYPE audition -> CC43=%u",
                 amp_type_id & 0x7F);
    } else {
        ESP_LOGE(TAG, "Amp Type CC43 failed: %s",
                 esp_err_to_name(rc));
    }

    return rc;
}

static void apply_snapshot_amp(uint8_t scene)
{
    scene &= 0x03;
    s_ui.scene = scene;
    s_ui.amp_editing = false;
    refresh_scene_amp_ui();
    render_ui();

    const snapshot_t *snap = snapshot_manager_get(scene);
    if (!snap || !snap->amp_configured) {
        ESP_LOGW(TAG,
                 "SCENE %c has no Amp configured; NANOCORE left unchanged",
                 (char)('A' + scene));
        return;
    }

    ESP_LOGI(TAG, "SCENE %c -> Amp Type ID %u only",
             (char)('A' + scene),
             snap->amp_type_id);

    if (send_amp_type(snap->amp_type_id) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(AMP_AUDITION_GAP_MS));
    }
}

static bool adv_has_midi_uuid(const uint8_t *adv, uint16_t adv_len)
{
    const uint8_t *p = NULL;
    uint8_t len = 0;

    if (esp_ble_conn_parse_adv_data(adv, adv_len,
                                    ESP_BLE_CONN_ADV_TYPE_UUID128_COMPLETE,
                                    &p, &len) == ESP_OK) {
        for (uint8_t off = 0; off + 16 <= len; off += 16) {
            if (memcmp(&p[off], MIDI_SERVICE_UUID, 16) == 0) {
                return true;
            }
        }
    }

    if (esp_ble_conn_parse_adv_data(adv, adv_len,
                                    ESP_BLE_CONN_ADV_TYPE_UUID128_INCOMP,
                                    &p, &len) == ESP_OK) {
        for (uint8_t off = 0; off + 16 <= len; off += 16) {
            if (memcmp(&p[off], MIDI_SERVICE_UUID, 16) == 0) {
                return true;
            }
        }
    }

    return false;
}

static void adv_get_name(const uint8_t *adv, uint16_t adv_len,
                         char *out, size_t out_len)
{
    if (!out || out_len == 0) return;

    out[0] = '\0';

    const uint8_t *name = NULL;
    uint8_t name_len = 0;

    esp_err_t rc = esp_ble_conn_parse_adv_data(
        adv, adv_len, ESP_BLE_CONN_ADV_TYPE_NAME_COMPLETE,
        &name, &name_len);

    if (rc != ESP_OK) {
        rc = esp_ble_conn_parse_adv_data(
            adv, adv_len, ESP_BLE_CONN_ADV_TYPE_NAME_SHORT,
            &name, &name_len);
    }

    if (rc == ESP_OK && name && name_len > 0) {
        size_t copy_len = name_len;
        if (copy_len >= out_len) copy_len = out_len - 1;
        memcpy(out, name, copy_len);
        out[copy_len] = '\0';
    }
}

static bool contains_nanocore(const char *name)
{
    if (!name || !name[0]) return false;

    char lower[48] = {0};
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;

    for (size_t i = 0; i < n; ++i) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }

    return strstr(lower, "nanocore") != NULL;
}

static bool scan_cb(const esp_ble_conn_scan_result_t *result, void *arg)
{
    (void)arg;

    if (!result || s_target_found) return false;

    char name[48];
    adv_get_name(result->adv_data, result->adv_data_len, name, sizeof(name));

    bool midi_uuid = adv_has_midi_uuid(result->adv_data,
                                       result->adv_data_len);
    bool nanocore_name = contains_nanocore(name);

    if (midi_uuid || nanocore_name) {
        ESP_LOGI(TAG,
                 "NANOCORE candidate: addr=" BLE_CONN_MGR_ADDR_STR
                 " RSSI=%d name=\"%s\" MIDI_UUID=%s",
                 BLE_CONN_MGR_ADDR_HEX(result->addr),
                 result->rssi,
                 name[0] ? name : "<no-name>",
                 midi_uuid ? "YES" : "no");

        s_target_found = true;
        ESP_LOGI(TAG, "Connecting...");
        return true;
    }

    return false;
}

static bool conn_data_is_midi_io(const esp_ble_conn_data_t *d)
{
    return d &&
           d->type == BLE_CONN_UUID_TYPE_128 &&
           memcmp(d->uuid.uuid128,
                  MIDI_CHAR_UUID,
                  sizeof(d->uuid.uuid128)) == 0;
}

static ui_block_state_t block_state_from_midi_value(uint8_t value)
{
    return value >= 64 ? UI_BLOCK_ON : UI_BLOCK_OFF;
}

static bool apply_received_block_cc(uint8_t cc, uint8_t value)
{
    ui_block_state_t state = block_state_from_midi_value(value);

    switch (cc) {
    case CC_FX1_ONOFF:
        s_ui.fx1 = state;
        return true;
    case CC_FX2_ONOFF:
        s_ui.fx2 = state;
        return true;
    case CC_AMP_ONOFF:
        s_ui.amp = state;
        return true;
    case CC_CAB_ONOFF:
        s_ui.cab = state;
        return true;
    case CC_MOD_ONOFF:
        s_ui.mod = state;
        return true;
    case CC_DELAY_ONOFF:
        s_ui.delay = state;
        return true;
    case CC_REVERB_ONOFF:
        s_ui.reverb = state;
        return true;
    case CC_EQ_ONOFF:
        s_ui.eq = state;
        return true;
    default:
        return false;
    }
}

static void midi_rx_cb(const uint8_t *data, uint16_t len, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGD(TAG, "BLE-MIDI RX packet: %u bytes", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
}

static void midi_evt_cb(uint16_t ts_ms,
                        esp_ble_midi_event_type_t event_type,
                        const uint8_t *msg,
                        uint16_t msg_len)
{
    if (event_type == ESP_BLE_MIDI_EVENT_SYSEX_OVERFLOW) {
        ESP_LOGW(TAG, "MIDI RX ts=%u: SysEx overflow", ts_ms);
        return;
    }

    if (!msg || msg_len == 0) {
        return;
    }

    const uint8_t status = msg[0] & 0xF0;

    if (status == 0xB0 && msg_len >= 3) {
        const uint8_t cc = msg[1] & 0x7F;
        const uint8_t value = msg[2] & 0x7F;

        ESP_LOGI(TAG,
                 "MIDI FEEDBACK RX: CC%u=%u -> %s",
                 cc,
                 value,
                 value >= 64 ? "ON/HIGH" : "OFF/LOW");

        if (apply_received_block_cc(cc, value)) {
            // Do not render from the BLE callback. Scene execution waits
            // briefly for feedback and renders from the button task.
        }
        return;
    }

    if (status == 0xC0 && msg_len >= 2) {
        ESP_LOGI(TAG, "MIDI FEEDBACK RX: Program Change %u",
                 msg[1] & 0x7F);
        return;
    }

    ESP_LOGI(TAG, "MIDI FEEDBACK RX: status=0x%02X len=%u",
             msg[0], msg_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, msg_len, ESP_LOG_INFO);
}

static void subscribe_midi_notifications(uint16_t conn_handle)
{
    static uint8_t cccd_notify[2] = {0x01, 0x00};

    esp_ble_conn_data_t sub = {
        .type = BLE_CONN_UUID_TYPE_128,
        .write_conn_id = conn_handle,
        .data = cccd_notify,
        .data_len = sizeof(cccd_notify),
    };
    memcpy(sub.uuid.uuid128, MIDI_CHAR_UUID, sizeof(sub.uuid.uuid128));

    esp_err_t rc = esp_ble_conn_subscribe_by_handle(
        conn_handle,
        ESP_BLE_CONN_DESC_CIENT_CONFIG,
        &sub);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Subscribed to BLE-MIDI notifications");
    } else {
        ESP_LOGW(TAG, "MIDI notification subscribe failed: %s",
                 esp_err_to_name(rc));
    }
}

static bool resolve_peer_midi_handle(uint16_t conn_handle)
{
    esp_ble_conn_uuid_t svc_uuid = {0};
    esp_ble_conn_uuid_t chr_uuid = {0};

    memcpy(svc_uuid.uuid128, MIDI_SERVICE_UUID, sizeof(svc_uuid.uuid128));
    memcpy(chr_uuid.uuid128, MIDI_CHAR_UUID, sizeof(chr_uuid.uuid128));

    s_midi_char_handle = 0;

    esp_err_t rc = esp_ble_conn_get_peer_chr_handle(
        conn_handle,
        BLE_CONN_UUID_TYPE_128,
        svc_uuid,
        BLE_CONN_UUID_TYPE_128,
        chr_uuid,
        &s_midi_char_handle);

    if (rc != ESP_OK || s_midi_char_handle == 0) {
        ESP_LOGE(TAG, "BLE-MIDI characteristic NOT found: %s",
                 esp_err_to_name(rc));
        return false;
    }

    ESP_LOGI(TAG, "BLE-MIDI characteristic found: handle=0x%04X",
             s_midi_char_handle);
    return true;
}

static esp_err_t send_current_program(void)
{
    if (!nanocore_midi_is_ready()) {
        ESP_LOGW(TAG, "MIDI not ready; PC %u not sent", s_ui.preset);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Program Change -> %u", s_ui.preset);

    esp_err_t rc = nanocore_midi_send_program_change(
        NANOCORE_MIDI_CHANNEL, s_ui.preset);

    if (rc == ESP_OK) {
        // Preset recall can change any block, so our local block cache is
        // deliberately invalidated until the controller sends a known state.
        set_blocks_unknown();
        render_ui();
    } else {
        ESP_LOGE(TAG, "Program Change failed: %s", esp_err_to_name(rc));
    }

    return rc;
}

static ui_block_state_t toggled_state(ui_block_state_t current)
{
    // First press from UNKNOWN intentionally sends ON.
    return current == UI_BLOCK_ON ? UI_BLOCK_OFF : UI_BLOCK_ON;
}

static esp_err_t set_block_cc(uint8_t cc,
                              ui_block_state_t *local_state,
                              ui_block_state_t new_state,
                              const char *label)
{
    if (!local_state) return ESP_ERR_INVALID_ARG;

    if (!nanocore_midi_is_ready()) {
        ESP_LOGW(TAG, "MIDI not ready; %s not changed", label);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t value = (new_state == UI_BLOCK_ON) ? 127 : 0;

    esp_err_t rc = nanocore_midi_send_cc(
        NANOCORE_MIDI_CHANNEL, cc, value);

    if (rc == ESP_OK) {
        *local_state = new_state;
        ESP_LOGI(TAG, "%s -> %s (CC%u=%u)",
                 label,
                 new_state == UI_BLOCK_ON ? "ON" : "OFF",
                 cc, value);
        render_ui();
    } else {
        ESP_LOGE(TAG, "%s CC failed: %s",
                 label, esp_err_to_name(rc));
    }

    return rc;
}

static void toggle_fx2(void)
{
    set_block_cc(CC_FX2_ONOFF,
                 &s_ui.fx2,
                 toggled_state(s_ui.fx2),
                 "FX2");
}

static void toggle_delay(void)
{
    set_block_cc(CC_DELAY_ONOFF,
                 &s_ui.delay,
                 toggled_state(s_ui.delay),
                 "DELAY");
}

static void cycle_mode(void)
{
    // Do not leave an amp audition half-edited.
    if (s_ui.amp_editing) {
        ESP_LOGW(TAG, "Finish AMP EDIT with ENC1 before changing mode");
        return;
    }

    s_ui.mode = (ui_mode_t)((s_ui.mode + 1) % UI_MODE_COUNT);

    ESP_LOGI(TAG, "MODE -> %s",
             s_ui.mode == UI_MODE_PRESET ? "PRESET" :
             s_ui.mode == UI_MODE_STOMP  ? "STOMP" : "SCENE");

    if (s_ui.mode == UI_MODE_SCENE) {
        apply_snapshot_amp(s_ui.scene);
    } else {
        render_ui();
    }
}

static void handle_program_click(void)
{
    switch (s_ui.mode) {
    case UI_MODE_STOMP:
        toggle_fx2();
        break;

    case UI_MODE_SCENE:
        if (s_ui.amp_editing) {
            ESP_LOGW(TAG, "ENC1: save AMP EDIT before changing Scene");
            break;
        }
        apply_snapshot_amp((uint8_t)((s_ui.scene + 3) & 0x03));
        break;

    case UI_MODE_PRESET:
    default:
        if (s_ui.preset <= NANOCORE_PRESET_MIN) {
            s_ui.preset = NANOCORE_PRESET_MAX;
        } else {
            --s_ui.preset;
        }
        send_current_program();
        break;
    }
}

static void handle_stop_click(void)
{
    switch (s_ui.mode) {
    case UI_MODE_STOMP:
        toggle_delay();
        break;

    case UI_MODE_SCENE:
        if (s_ui.amp_editing) {
            ESP_LOGW(TAG, "ENC1: save AMP EDIT before changing Scene");
            break;
        }
        apply_snapshot_amp((uint8_t)((s_ui.scene + 1) & 0x03));
        break;

    case UI_MODE_PRESET:
    default:
        if (s_ui.preset >= NANOCORE_PRESET_MAX) {
            s_ui.preset = NANOCORE_PRESET_MIN;
        } else {
            ++s_ui.preset;
        }
        send_current_program();
        break;
    }
}

static void ble_event_handler(void *handler_args,
                              esp_event_base_t base,
                              int32_t id,
                              void *event_data)
{
    (void)handler_args;

    if (base != BLE_CONN_MGR_EVENTS) return;

    switch (id) {
    case ESP_BLE_CONN_EVENT_STARTED:
        ESP_LOGI(TAG, "BLE manager started; scanning...");
        render_ui();
        break;

    case ESP_BLE_CONN_EVENT_CONNECTED:
        if (esp_ble_conn_get_conn_handle(&s_conn_handle) != ESP_OK ||
            s_conn_handle == BLE_CONN_HANDLE_INVALID) {
            ESP_LOGE(TAG, "Connected event but no valid handle");
            break;
        }

        s_ui.ble_connected = true;
        ESP_LOGI(TAG, "CONNECTED: conn_handle=%u", s_conn_handle);
        render_ui();

        {
            esp_err_t mtu_rc = esp_ble_conn_mtu_update(s_conn_handle, 185);
            if (mtu_rc != ESP_OK) {
                ESP_LOGW(TAG, "MTU request failed: %s",
                         esp_err_to_name(mtu_rc));
            }
        }
        break;

    case ESP_BLE_CONN_EVENT_MTU: {
        esp_ble_conn_event_data_t *evt =
            (esp_ble_conn_event_data_t *)event_data;
        if (evt) {
            ESP_LOGI(TAG, "MTU negotiated: %u", evt->mtu_update.mtu);
        }
        break;
    }

    case ESP_BLE_CONN_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "GATT discovery complete");

        if (s_conn_handle != BLE_CONN_HANDLE_INVALID &&
            resolve_peer_midi_handle(s_conn_handle)) {

            subscribe_midi_notifications(s_conn_handle);
            nanocore_midi_set_link(s_conn_handle, s_midi_char_handle);

            s_ui.midi_ready = true;
            render_ui();

            ESP_LOGI(TAG, "======================================");
            ESP_LOGI(TAG, " STAGE 04 READY");
            ESP_LOGI(TAG, " PRESET/STOMP/SCENE + LANDSCAPE UI");
            ESP_LOGI(TAG, " HOLD GPIO18 + GPIO21 = MODE");
            ESP_LOGI(TAG, " GPIO47 = MODE shortcut");
            ESP_LOGI(TAG, " SCENE: ENC1 press=AMP EDIT, turn=CC43, press=SAVE");
            ESP_LOGI(TAG, "======================================");
        }
        break;

    case ESP_BLE_CONN_EVENT_DATA_RECEIVE: {
        esp_ble_conn_data_t *conn_data =
            (esp_ble_conn_data_t *)event_data;

        if (conn_data &&
            conn_data->data &&
            conn_data->data_len > 0 &&
            conn_data_is_midi_io(conn_data)) {
            esp_ble_midi_on_bep_received(
                conn_data->data,
                conn_data->data_len);
        }
        break;
    }

    case ESP_BLE_CONN_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "DISCONNECTED; restarting scan");

        nanocore_midi_clear_link();
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_midi_char_handle = 0;
        s_target_found = false;

        s_ui.ble_connected = false;
        s_ui.midi_ready = false;
        set_blocks_unknown();
        render_ui();

        if (esp_ble_conn_scan_start() != ESP_OK) {
            ESP_LOGW(TAG, "Could not restart scan");
        }
        break;

    default:
        break;
    }
}

typedef struct {
    gpio_num_t pin_a;
    gpio_num_t pin_b;
    gpio_num_t pin_sw;
    int last_a;
    bool last_sw_raw;
    bool sw_stable;
    TickType_t sw_last_change;
} encoder_state_t;

static int encoder_step(encoder_state_t *e)
{
    int a = gpio_get_level(e->pin_a);
    int step = 0;

    // Same proven direction rule used by the original programmer firmware:
    // count one detent on the falling edge of A.
    if (a != e->last_a && a == 0) {
        int b = gpio_get_level(e->pin_b);
        step = (b != a) ? 1 : -1;
    }

    e->last_a = a;
    return step;
}

static bool encoder_click(encoder_state_t *e, TickType_t now)
{
    bool raw = gpio_get_level(e->pin_sw) == 0;

    if (raw != e->last_sw_raw) {
        e->last_sw_raw = raw;
        e->sw_last_change = now;
    }

    if ((now - e->sw_last_change) < pdMS_TO_TICKS(35)) {
        return false;
    }

    if (raw == e->sw_stable) {
        return false;
    }

    e->sw_stable = raw;
    return raw; // press edge
}

static void start_amp_edit(void)
{
    if (s_ui.mode != UI_MODE_SCENE) {
        return;
    }

    refresh_scene_amp_ui();

    s_ui.amp_editing = true;
    s_ui.amp_edit_candidate = s_ui.scene_amp_configured
                                ? s_ui.scene_amp_type : 0;

    ESP_LOGI(TAG,
             "AMP EDIT Scene %c started at ID %u%s",
             (char)('A' + s_ui.scene),
             s_ui.amp_edit_candidate,
             s_ui.scene_amp_configured ? "" : " (default audition start)");

    // Audition the starting point so what is shown is what is heard.
    if (send_amp_type(s_ui.amp_edit_candidate) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(AMP_AUDITION_GAP_MS));
    }

    render_ui();
}

static void save_amp_edit(void)
{
    esp_err_t rc = snapshot_manager_set_amp(
        s_ui.scene,
        s_ui.amp_edit_candidate);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Scene %c Amp ID %u: %s",
                 (char)('A' + s_ui.scene),
                 s_ui.amp_edit_candidate,
                 esp_err_to_name(rc));
        return;
    }

    ESP_LOGI(TAG, "AMP EDIT Scene %c saved -> ID %u",
             (char)('A' + s_ui.scene),
             s_ui.amp_edit_candidate);

    s_ui.amp_editing = false;
    refresh_scene_amp_ui();
    render_ui();
}

static void handle_amp_encoder_step(int step)
{
    if (!s_ui.amp_editing || step == 0) {
        return;
    }

    int value = (int)s_ui.amp_edit_candidate + step;

    // Clamp rather than wrap; this makes exploring unknown valid ID ranges
    // safer and preserves the user's perceived ordering.
    if (value < 0) value = 0;
    if (value > 127) value = 127;

    if (value == s_ui.amp_edit_candidate) {
        return;
    }

    s_ui.amp_edit_candidate = (uint8_t)value;
    render_ui();

    if (send_amp_type(s_ui.amp_edit_candidate) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(AMP_AUDITION_GAP_MS));
    }
}

typedef struct {
    gpio_num_t gpio;
    bool stable_pressed;
    bool raw_pressed;
    bool consumed;
    bool click;
    TickType_t last_raw_change;
    TickType_t press_tick;
} button_state_t;

static void button_poll(button_state_t *b, TickType_t now)
{
    b->click = false;

    bool raw = gpio_get_level(b->gpio) == 0;

    if (raw != b->raw_pressed) {
        b->raw_pressed = raw;
        b->last_raw_change = now;
    }

    if ((now - b->last_raw_change) < pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        return;
    }

    if (raw == b->stable_pressed) {
        return;
    }

    b->stable_pressed = raw;

    if (raw) {
        b->press_tick = now;
        b->consumed = false;
    } else {
        if (!b->consumed) {
            b->click = true;
        }
    }
}

static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask =
            (1ULL << BTN_PROGRAM) |
            (1ULL << BTN_STOP) |
            (1ULL << BTN_FUNCTION) |
            (1ULL << ENC1_A) |
            (1ULL << ENC1_B) |
            (1ULL << ENC1_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void button_task(void *arg)
{
    (void)arg;

    TickType_t now = xTaskGetTickCount();

    button_state_t program = {
        .gpio = BTN_PROGRAM,
        .last_raw_change = now,
    };

    button_state_t stop = {
        .gpio = BTN_STOP,
        .last_raw_change = now,
    };

    button_state_t function = {
        .gpio = BTN_FUNCTION,
        .last_raw_change = now,
    };

    encoder_state_t enc1 = {
        .pin_a = ENC1_A,
        .pin_b = ENC1_B,
        .pin_sw = ENC1_SW,
        .last_a = gpio_get_level(ENC1_A),
        .last_sw_raw = false,
        .sw_stable = false,
        .sw_last_change = now,
    };

    bool chord_active = false;
    bool chord_fired = false;
    TickType_t chord_start = 0;

    while (1) {
        now = xTaskGetTickCount();

        button_poll(&program, now);
        button_poll(&stop, now);
        button_poll(&function, now);

        int enc_step = encoder_step(&enc1);
        if (enc_step != 0 && s_ui.mode == UI_MODE_SCENE) {
            handle_amp_encoder_step(enc_step);
        }

        if (encoder_click(&enc1, now) && s_ui.mode == UI_MODE_SCENE) {
            if (s_ui.amp_editing) {
                save_amp_edit();
            } else {
                start_amp_edit();
            }
        }

        if (program.stable_pressed && stop.stable_pressed) {
            if (!chord_active) {
                chord_active = true;
                chord_fired = false;
                chord_start = now;
            }

            if (!chord_fired &&
                (now - chord_start) >= pdMS_TO_TICKS(MODE_CHORD_HOLD_MS)) {

                program.consumed = true;
                stop.consumed = true;
                chord_fired = true;

                cycle_mode();
            }
        } else if (!program.stable_pressed && !stop.stable_pressed) {
            chord_active = false;
            chord_fired = false;
        }

        // Individual commands execute on RELEASE so the two-button chord
        // never accidentally changes a preset/effect before mode switching.
        if (program.click) {
            handle_program_click();
        }

        if (stop.click) {
            handle_stop_click();
        }

        if (function.click) {
            if (s_ui.amp_editing) {
                ESP_LOGW(TAG, "AMP EDIT active; press ENC1 to save first");
            } else {
                cycle_mode();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void nvs_init_safe(void)
{
    esp_err_t rc = nvs_flash_init();

    if (rc == ESP_ERR_NVS_NO_FREE_PAGES ||
        rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }

    ESP_ERROR_CHECK(rc);
}

void app_main(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, " NANOCORE MIDI CONTROLLER - STAGE 04");
    ESP_LOGI(TAG, " LANDSCAPE UI + PERFORMANCE MODES");
    ESP_LOGI(TAG, "======================================");

    nvs_init_safe();
    ESP_ERROR_CHECK(snapshot_manager_init());
    buttons_init();

    refresh_scene_amp_ui();

    esp_err_t display_rc = display_ui_init();
    if (display_rc == ESP_OK) {
        render_ui();
    } else {
        // BLE/MIDI remains testable over serial even if the TFT needs
        // an orientation/color/driver adjustment on the physical unit.
        ESP_LOGE(TAG, "Display init failed: %s",
                 esp_err_to_name(display_rc));
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(
        BLE_CONN_MGR_EVENTS,
        ESP_EVENT_ANY_ID,
        ble_event_handler,
        NULL));

    esp_ble_conn_config_t config = {0};
    strncpy((char *)config.device_name,
            "Nanocore_Controller",
            sizeof(config.device_name) - 1);

    ESP_ERROR_CHECK(esp_ble_conn_init(&config));
    ESP_ERROR_CHECK(esp_ble_conn_register_scan_callback(scan_cb, NULL));

    esp_ble_conn_scan_params_t scan_params = {
        .passive = false,
        .filter_duplicates = true,
    };
    ESP_ERROR_CHECK(esp_ble_conn_scan_params_set(&scan_params));

    ESP_ERROR_CHECK(esp_ble_midi_svc_init());
    ESP_ERROR_CHECK(esp_ble_midi_profile_init());
    ESP_ERROR_CHECK(esp_ble_midi_register_rx_cb(midi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_ble_midi_register_event_cb(midi_evt_cb));

    ESP_ERROR_CHECK(esp_ble_conn_start());

    xTaskCreate(
        button_task,
        "nc_buttons",
        4096,
        NULL,
        5,
        NULL);

    ESP_LOGI(TAG, "Waiting for NANOCORE BLE-MIDI...");
}
