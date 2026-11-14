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

static const char *TAG = "NC_STAGE04";

static const uint8_t MIDI_SERVICE_UUID[16] = BLE_MIDI_SERVICE_UUID128;
static const uint8_t MIDI_CHAR_UUID[16]    = BLE_MIDI_CHAR_UUID128;

#define NANOCORE_MIDI_CHANNEL       1
#define NANOCORE_PRESET_MIN         0
#define NANOCORE_PRESET_MAX        63

#define CC_FX2_ONOFF               21
#define CC_MOD_ONOFF               25
#define CC_DELAY_ONOFF             26
#define CC_REVERB_ONOFF            27

#define BUTTON_POLL_MS             10
#define BUTTON_DEBOUNCE_MS         35
#define MODE_CHORD_HOLD_MS        700

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

    if (msg && msg_len > 0) {
        ESP_LOGD(TAG, "MIDI RX decoded ts=%u len=%u", ts_ms, msg_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, msg_len, ESP_LOG_DEBUG);
    }
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

typedef struct {
    bool fx2;
    bool mod;
    bool delay;
    bool reverb;
    const char *name;
} demo_scene_t;

static const demo_scene_t DEMO_SCENES[4] = {
    {false, false, false, true,  "CLEAN"},
    {true,  false, false, true,  "RHYTHM"},
    {true,  false, true,  true,  "LEAD"},
    {false, true,  true,  true,  "AMBIENT"},
};

static void apply_demo_scene(uint8_t scene)
{
    scene &= 0x03;

    if (!nanocore_midi_is_ready()) {
        ESP_LOGW(TAG, "MIDI not ready; scene not applied");
        return;
    }

    const demo_scene_t *sc = &DEMO_SCENES[scene];

    struct {
        uint8_t cc;
        bool on;
    } actions[] = {
        {CC_FX2_ONOFF, sc->fx2},
        {CC_MOD_ONOFF, sc->mod},
        {CC_DELAY_ONOFF, sc->delay},
        {CC_REVERB_ONOFF, sc->reverb},
    };

    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        esp_err_t rc = nanocore_midi_send_cc(
            NANOCORE_MIDI_CHANNEL,
            actions[i].cc,
            actions[i].on ? 127 : 0);

        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "Scene action CC%u failed: %s",
                     actions[i].cc, esp_err_to_name(rc));
            return;
        }

        // Avoid an unnecessary burst while we validate the transport.
        vTaskDelay(pdMS_TO_TICKS(4));
    }

    s_ui.scene = scene;
    s_ui.fx2 = sc->fx2 ? UI_BLOCK_ON : UI_BLOCK_OFF;
    s_ui.mod = sc->mod ? UI_BLOCK_ON : UI_BLOCK_OFF;
    s_ui.delay = sc->delay ? UI_BLOCK_ON : UI_BLOCK_OFF;
    s_ui.reverb = sc->reverb ? UI_BLOCK_ON : UI_BLOCK_OFF;

    ESP_LOGI(TAG, "SCENE %c / %s applied",
             (char)('A' + scene), sc->name);

    render_ui();
}

static void cycle_mode(void)
{
    s_ui.mode = (ui_mode_t)((s_ui.mode + 1) % UI_MODE_COUNT);

    ESP_LOGI(TAG, "MODE -> %s",
             s_ui.mode == UI_MODE_PRESET ? "PRESET" :
             s_ui.mode == UI_MODE_STOMP  ? "STOMP" : "SCENE");

    render_ui();
}

static void handle_program_click(void)
{
    switch (s_ui.mode) {
    case UI_MODE_STOMP:
        toggle_fx2();
        break;

    case UI_MODE_SCENE:
        s_ui.scene = (uint8_t)((s_ui.scene + 3) & 0x03);
        apply_demo_scene(s_ui.scene);
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
        s_ui.scene = (uint8_t)((s_ui.scene + 1) & 0x03);
        apply_demo_scene(s_ui.scene);
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
            (1ULL << BTN_FUNCTION),
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

    bool chord_active = false;
    bool chord_fired = false;
    TickType_t chord_start = 0;

    while (1) {
        now = xTaskGetTickCount();

        button_poll(&program, now);
        button_poll(&stop, now);
        button_poll(&function, now);

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
            cycle_mode();
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
    buttons_init();

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
