/*
 * Nanocore MIDI Controller - Stage 07 / Full Dynamic Editor
 *
 * ESP32-S3 N16R8 + ST7796S 3.5" + 2 encoders + 5 footswitches + 2 LEDs.
 *
 * Responsibilities are deliberately split into components:
 * board / input_manager / display_ui / controller_storage / tempo_engine /
 * status_leds / nanocore_map / name_editor / nanocore_midi.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_conn_mgr.h"
#include "esp_ble_midi.h"
#include "esp_ble_midi_svc.h"

#include "board.h"
#include "controller_storage.h"
#include "controller_types.h"
#include "display_ui.h"
#include "input_manager.h"
#include "name_editor.h"
#include "nanocore_map.h"
#include "nanocore_mapper.h"
#include "nanocore_midi.h"
#include "nanocore_protocol.h"
#include "nanocore_spec.h"
#include "status_leds.h"
#include "tempo_engine.h"

static const char *TAG = "NC_STAGE07";

static const uint8_t MIDI_SERVICE_UUID[16] = BLE_MIDI_SERVICE_UUID128;
static const uint8_t MIDI_CHAR_UUID[16]    = BLE_MIDI_CHAR_UUID128;

#define MIDI_CHANNEL              1
#define NANOCORE_PRESET_MIN       0
#define NANOCORE_PRESET_MAX      63

#define CC_DELAY_TIME            74
#define CC_TUNER                 80

#define MIDI_ACTION_GAP_MS       28
#define RECALL_FIRST_SYNC_MS    600
#define RECALL_RETRY_MS         250
#define RECALL_REPLY_TIMEOUT_MS 900
#define RECALL_TIMEOUT_MS      3500
#define NOTICE_MS              1800

static bool s_target_found;
static bool s_ble_connected;
static bool s_midi_ready;
static bool s_dirty;
static bool s_tuner_on;

static bool s_block_known[BLOCK_COUNT];
static bool s_block_on[BLOCK_COUNT];
static bool s_block_profile_known[BLOCK_COUNT];
static uint8_t s_block_variant[BLOCK_COUNT];
static uint8_t s_block_param_count[BLOCK_COUNT];
static float s_block_params[BLOCK_COUNT][NC_PROTO_MAX_PARAMS];

static bool s_amp_model_known;
static bool s_cab_model_known;
static uint8_t s_amp_model;
static uint8_t s_cab_model;

static char s_notice[64];
static int64_t s_notice_until_us;

/*
 * Program Change is asynchronous inside NanoCore. A live-state read sent
 * too early may legitimately return the previous slot. Keep the controller
 * target authoritative until opcode 0x63 confirms that exact slot.
 */
static bool s_recall_pending;
static bool s_recall_poll_inflight;
static bool s_name_after_live;
static bool s_editor_models_after_live;
static uint8_t s_recall_target;
static uint8_t s_recall_poll_count;
static int64_t s_recall_started_us;
static int64_t s_recall_next_poll_us;
static int64_t s_recall_poll_sent_us;

static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint16_t s_midi_char_handle;

static controller_runtime_t s_runtime;
static controller_preset_t s_preset;

static controller_block_t s_selected_block = BLOCK_FX1;
static size_t s_selected_parameter;
static bool s_parameter_editing;

static ui_screen_t s_screen = UI_SCREEN_PERFORMANCE;

static name_editor_t s_name_editor;
static bool s_name_target_scene;

static void render_ui(void);

static void show_notice(const char *text)
{
    if (!text) return;
    strncpy(s_notice, text, sizeof(s_notice) - 1);
    s_notice[sizeof(s_notice) - 1] = '\0';
    s_notice_until_us = esp_timer_get_time() + (NOTICE_MS * 1000LL);
    render_ui();
}

static void clear_block_state(void)
{
    memset(s_block_known, 0, sizeof(s_block_known));
    memset(s_block_on, 0, sizeof(s_block_on));
    memset(s_block_profile_known, 0, sizeof(s_block_profile_known));
    memset(s_block_variant, 0, sizeof(s_block_variant));
    memset(s_block_param_count, 0, sizeof(s_block_param_count));
    memset(s_block_params, 0, sizeof(s_block_params));
    s_amp_model_known = false;
    s_cab_model_known = false;
    s_amp_model = 0;
    s_cab_model = 0;
}

static int block_from_onoff_cc(uint8_t cc)
{
    for (int i = 0; i < BLOCK_COUNT; ++i) {
        if (nanocore_block_onoff_cc((controller_block_t)i) == cc) return i;
    }
    return -1;
}

static int block_from_type_cc(uint8_t cc)
{
    for (int i = 0; i < BLOCK_COUNT; ++i) {
        if (nanocore_block_type_cc((controller_block_t)i) == cc) return i;
    }
    return -1;
}

static void schedule_recall_sync(uint8_t target)
{
    s_recall_pending = true;
    s_recall_poll_inflight = false;
    s_name_after_live = true;
    s_recall_target = target;
    s_recall_poll_count = 0;
    s_recall_started_us = esp_timer_get_time();
    s_recall_next_poll_us =
        s_recall_started_us + (RECALL_FIRST_SYNC_MS * 1000LL);

    ESP_LOGI(TAG, "Recall sync armed: target slot=%u", target);
}

static void service_recall_sync(void)
{
    if (!s_recall_pending || !s_midi_ready) return;

    int64_t now = esp_timer_get_time();

    if ((now - s_recall_started_us) >=
        (RECALL_TIMEOUT_MS * 1000LL)) {
        ESP_LOGE(TAG,
                 "Recall sync timeout: target=%u polls=%u",
                 s_recall_target, s_recall_poll_count);
        s_recall_pending = false;
        show_notice("NANOCORE PRESET SYNC TIMEOUT");
        return;
    }

    if (s_recall_poll_inflight) {
        if ((now - s_recall_poll_sent_us) <
            (RECALL_REPLY_TIMEOUT_MS * 1000LL)) {
            return;
        }

        ESP_LOGW(TAG,
                 "Recall sync reply timeout; retrying target=%u",
                 s_recall_target);
        s_recall_poll_inflight = false;
        s_recall_next_poll_us = now;
    }

    if (now < s_recall_next_poll_us) return;

    esp_err_t rc = nanocore_protocol_request_live_state();
    if (rc == ESP_OK) {
        ++s_recall_poll_count;
        s_recall_poll_inflight = true;
        s_recall_poll_sent_us = now;
        ESP_LOGI(TAG,
                 "Recall sync poll #%u target=%u",
                 s_recall_poll_count, s_recall_target);
    } else {
        ESP_LOGW(TAG, "Recall sync poll failed: %s",
                 esp_err_to_name(rc));
        s_recall_next_poll_us =
            now + (RECALL_RETRY_MS * 1000LL);
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

static esp_err_t persist_runtime(void)
{
    esp_err_t rc = controller_storage_save_runtime(&s_runtime);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "Runtime NVS save failed: %s", esp_err_to_name(rc));
    }
    return rc;
}

static midi_assignment_t *edit_assignments(size_t *count)
{
    if (s_runtime.mode == CTRL_MODE_SCENE) {
        if (count) *count = CTRL_SCENE_ASSIGNMENTS;
        return s_preset.scene[s_runtime.current_scene].assignments;
    }

    if (count) *count = CTRL_BASE_ASSIGNMENTS;
    return s_preset.base;
}

static const midi_assignment_t *scene_assignments(uint8_t scene,
                                                   size_t *count)
{
    if (scene >= CTRL_SCENE_COUNT) return NULL;
    if (count) *count = CTRL_SCENE_ASSIGNMENTS;
    return s_preset.scene[scene].assignments;
}

static bool effective_cc(uint8_t cc, uint8_t *value)
{
    if (s_runtime.mode == CTRL_MODE_SCENE) {
        const midi_assignment_t *scene =
            s_preset.scene[s_runtime.current_scene].assignments;

        if (controller_assignment_get(scene,
                                      CTRL_SCENE_ASSIGNMENTS,
                                      cc,
                                      value)) {
            return true;
        }
    }

    return controller_assignment_get(s_preset.base,
                                     CTRL_BASE_ASSIGNMENTS,
                                     cc,
                                     value);
}

static esp_err_t send_cc(uint8_t cc, uint8_t value)
{
    if (!nanocore_midi_is_ready()) return ESP_ERR_INVALID_STATE;

    esp_err_t rc = nanocore_midi_send_cc(MIDI_CHANNEL, cc, value);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "CC%u=%u", cc, value);
    }
    return rc;
}

static const nanocore_effect_type_info_t *current_type_info(
    controller_block_t block)
{
    if (block == BLOCK_AMP || block == BLOCK_CAB) return NULL;
    if (block >= BLOCK_COUNT || !s_block_profile_known[block]) return NULL;
    return nanocore_spec_block_type(block, s_block_variant[block]);
}

static const nanocore_param_info_t *current_param_spec(
    controller_block_t block, uint8_t *count)
{
    if (count) *count = 0;

    if (block == BLOCK_AMP || block == BLOCK_CAB) {
        return nanocore_spec_common_params(block, count);
    }

    const nanocore_effect_type_info_t *info =
        current_type_info(block);
    if (!info || !info->params) return NULL;

    if (count) *count = info->expected_param_count;
    return info->params;
}

static bool current_selector_index(controller_block_t block, int *index)
{
    if (!index) return false;

    if (block == BLOCK_AMP) {
        if (!s_amp_model_known) return false;
        *index = s_amp_model;
        return true;
    }

    if (block == BLOCK_CAB) {
        if (!s_cab_model_known) return false;
        *index = s_cab_model;
        return true;
    }

    if (block >= BLOCK_COUNT || !s_block_profile_known[block]) return false;

    int i = nanocore_spec_block_selector_index(
        block, s_block_variant[block]);
    if (i < 0) return false;

    *index = i;
    return true;
}

static const char *selector_name(controller_block_t block, size_t index)
{
    if (block == BLOCK_AMP)
        return nanocore_spec_amp_model_name((uint8_t)index);
    if (block == BLOCK_CAB)
        return nanocore_spec_cab_model_name((uint8_t)index);

    const nanocore_effect_type_info_t *info =
        nanocore_spec_block_selector(block, index);
    return info ? info->name : NULL;
}

static size_t editor_parameter_count(void)
{
    uint8_t count = 0;
    (void)current_param_spec(s_selected_block, &count);
    return 2 + count; /* ON/OFF + TYPE/MODEL + live params */
}

static esp_err_t set_profile_param(controller_block_t block,
                                   uint8_t param_index,
                                   float normalized)
{
    uint8_t count = 0;
    const nanocore_param_info_t *params =
        current_param_spec(block, &count);

    if (!params || param_index >= count ||
        param_index >= NC_PROTO_MAX_PARAMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    esp_err_t rc = nanocore_protocol_set_block_param(
        block, param_index, normalized);

    if (rc == ESP_OK) {
        s_block_profile_known[block] = true;
        if (s_block_param_count[block] < count)
            s_block_param_count[block] = count;
        s_block_params[block][param_index] = normalized;
        s_dirty = true;

        float real = nanocore_spec_normalized_to_real(
            &params[param_index], normalized);

        ESP_LOGI(TAG,
                 "%s param %u %s -> norm=%.6f real=%.3f%s",
                 nanocore_block_name(block),
                 param_index,
                 params[param_index].name,
                 (double)normalized,
                 (double)real,
                 params[param_index].unit ? params[param_index].unit : "");
    }

    return rc;
}

static esp_err_t send_type_or_model_raw(controller_block_t block,
                                        uint8_t raw_type)
{
    if (block == BLOCK_AMP) {
        esp_err_t rc = nanocore_protocol_set_amp_model(raw_type);
        if (rc == ESP_OK) {
            s_amp_model_known = true;
            s_amp_model = raw_type;
            s_dirty = true;
        }
        return rc;
    }

    if (block == BLOCK_CAB) {
        esp_err_t rc = nanocore_protocol_set_cab_model(raw_type);
        if (rc == ESP_OK) {
            s_cab_model_known = true;
            s_cab_model = raw_type;
            s_dirty = true;
        }
        return rc;
    }

    const nanocore_effect_type_info_t *info =
        nanocore_spec_block_type(block, raw_type);
    if (!info) return ESP_ERR_INVALID_ARG;

    float defaults[NC_PROTO_MAX_PARAMS] = {0};
    if (!nanocore_spec_fill_type_defaults(
            block, raw_type, defaults, NC_PROTO_MAX_PARAMS)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = nanocore_protocol_set_block_type(
        block, raw_type, defaults, info->expected_param_count);

    if (rc == ESP_OK) {
        s_block_profile_known[block] = true;
        s_block_variant[block] = raw_type;
        s_block_param_count[block] = info->expected_param_count;
        memset(s_block_params[block], 0,
               sizeof(s_block_params[block]));
        memcpy(s_block_params[block], defaults,
               (size_t)info->expected_param_count * sizeof(float));
        s_dirty = true;

        ESP_LOGI(TAG,
                 "%s TYPE -> %s (raw=%u params=%u)",
                 nanocore_block_name(block),
                 info->name, raw_type, info->expected_param_count);
    }

    return rc;
}

static esp_err_t set_working_cc(uint8_t cc, uint8_t value)
{
    size_t count = 0;
    midi_assignment_t *items = edit_assignments(&count);

    esp_err_t rc = controller_assignment_set(items, count, cc, value);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "No room for CC%u in working context", cc);
        return rc;
    }

    s_dirty = true;

    int block = block_from_onoff_cc(cc);
    if (block >= 0) {
        s_block_known[block] = true;
        s_block_on[block] = value >= 64;
    }

    if (s_midi_ready) {
        esp_err_t tx;
        int type_block = block_from_type_cc(cc);

        if (type_block >= 0) {
            tx = send_type_or_model_raw(
                (controller_block_t)type_block, value);
        } else {
            tx = send_cc(cc, value);
        }

        if (tx != ESP_OK) {
            ESP_LOGW(TAG,
                     "CC%u/raw=%u stored locally; live TX failed",
                     cc, value);
        }
    } else {
        ESP_LOGI(TAG, "CC%u=%u stored locally (MIDI offline)", cc, value);
    }

    render_ui();
    return ESP_OK;
}

static void apply_assignments(const midi_assignment_t *items,
                              size_t count)
{
    if (!items || !s_midi_ready) return;

    for (size_t i = 0; i < count; ++i) {
        if (!items[i].used) continue;

        int type_block = block_from_type_cc(items[i].cc);
        esp_err_t tx = type_block >= 0
            ? send_type_or_model_raw(
                (controller_block_t)type_block, items[i].value)
            : send_cc(items[i].cc, items[i].value);

        if (tx == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(MIDI_ACTION_GAP_MS));
        }
    }
}

static void apply_current_scene(void)
{
    if (s_runtime.current_scene >= CTRL_SCENE_COUNT) {
        s_runtime.current_scene = 0;
    }

    s_preset.active_scene = s_runtime.current_scene;

    if (s_midi_ready) {
        size_t count = 0;
        const midi_assignment_t *items =
            scene_assignments(s_runtime.current_scene, &count);

        ESP_LOGI(TAG, "Apply Scene %c / %s",
                 (char)('A' + s_runtime.current_scene),
                 s_preset.scene[s_runtime.current_scene].name);
        apply_assignments(items, count);
    }

    (void)persist_runtime();
    render_ui();
}

static void restore_current_preset_to_nanocore(void)
{
    if (!s_midi_ready) return;

    const uint8_t target = s_runtime.current_preset;

    ESP_LOGI(TAG, "Recall PC=%u mode=%u scene=%u",
             target,
             (unsigned)s_runtime.mode,
             s_runtime.current_scene);

    clear_block_state();
    render_ui();

    // Arm the expected slot BEFORE sending PC so any immediate/odd push
    // generated by this recall cannot move the controller backwards.
    schedule_recall_sync(target);

    esp_err_t rc = nanocore_midi_send_program_change(
        MIDI_CHANNEL, target);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Program Change failed: %s",
                 esp_err_to_name(rc));
        s_recall_pending = false;
        s_recall_poll_inflight = false;
        show_notice("PROGRAM CHANGE FAILED");
    }

    /*
     * Do not apply Scene overrides or request 0x63 immediately here.
     * NanoCore can need ~400-600 ms before its live profile changes slot.
     * The asynchronous recall-sync state machine waits for LIVE slot=target.
     */
}

static esp_err_t load_preset(uint8_t index, bool transmit)
{
    if (index >= CTRL_PRESET_COUNT) return ESP_ERR_INVALID_ARG;

    controller_preset_t loaded;
    esp_err_t rc = controller_storage_load_preset(index, &loaded);
    if (rc != ESP_OK) return rc;

    s_preset = loaded;
    s_runtime.current_preset = index;
    s_runtime.current_scene =
        s_preset.active_scene < CTRL_SCENE_COUNT
            ? s_preset.active_scene : 0;

    tempo_engine_set_bpm(s_preset.bpm);
    status_leds_set_bpm(s_preset.bpm);

    s_dirty = false;
    s_parameter_editing = false;
    s_selected_parameter = 0;
    s_screen = UI_SCREEN_PERFORMANCE;

    (void)persist_runtime();

    if (transmit) {
        restore_current_preset_to_nanocore();
    }

    render_ui();
    return ESP_OK;
}

static void change_preset(int delta)
{
    if (s_dirty) {
        ESP_LOGW(TAG, "Preset change blocked: unsaved edits. Use SAVE chord.");
        show_notice("SAVE CHANGES BEFORE PRESET +/-");
        return;
    }

    int p = (int)s_runtime.current_preset + delta;
    if (p < NANOCORE_PRESET_MIN) p = NANOCORE_PRESET_MAX;
    if (p > NANOCORE_PRESET_MAX) p = NANOCORE_PRESET_MIN;

    ESP_LOGI(TAG, "Preset -> %d", p);
    (void)load_preset((uint8_t)p, true);
}

static void change_scene(int delta)
{
    int s = (int)s_runtime.current_scene + delta;
    while (s < 0) s += CTRL_SCENE_COUNT;
    while (s >= CTRL_SCENE_COUNT) s -= CTRL_SCENE_COUNT;

    s_runtime.current_scene = (uint8_t)s;
    apply_current_scene();
}

static void cycle_mode(void)
{
    s_parameter_editing = false;
    s_screen = UI_SCREEN_PERFORMANCE;

    controller_mode_t previous = s_runtime.mode;
    s_runtime.mode =
        (controller_mode_t)((s_runtime.mode + 1) % CTRL_MODE_COUNT);

    status_leds_set_mode(s_runtime.mode);
    (void)persist_runtime();

    ESP_LOGI(TAG, "MODE -> %u", (unsigned)s_runtime.mode);

    if (s_runtime.mode == CTRL_MODE_SCENE) {
        apply_current_scene();
    } else if (previous == CTRL_MODE_SCENE) {
        // Leaving Scene must remove its temporary overrides and return to
        // the saved physical NanoCore preset.
        restore_current_preset_to_nanocore();
    } else {
        render_ui();
    }
}

static void update_delay_from_tempo(void)
{
    uint32_t delay_ms = tempo_engine_delay_ms(
        s_preset.bpm,
        s_preset.delay_subdivision);

    uint8_t cc74 = tempo_engine_delay_ms_to_cc74(delay_ms);

    ESP_LOGI(TAG,
             "Delay tempo: %u BPM %s -> %lu ms -> CC74=%u",
             s_preset.bpm,
             tempo_engine_subdivision_name(s_preset.delay_subdivision),
             (unsigned long)delay_ms,
             cc74);

    (void)set_working_cc(CC_DELAY_TIME, cc74);
}

static void register_tap(void)
{
    status_leds_tap_feedback();

    bool updated = tempo_engine_tap();
    if (updated) {
        s_preset.bpm = tempo_engine_get_bpm();
        status_leds_set_bpm(s_preset.bpm);
        s_dirty = true;
        update_delay_from_tempo();
    } else {
        render_ui();
    }
}

static void toggle_tap_capture(void)
{
    if (tempo_engine_tap_active()) {
        tempo_engine_cancel_tap();
        status_leds_set_tap_capture(false);
        ESP_LOGI(TAG, "Tap capture OFF");
    } else {
        tempo_engine_begin_tap();
        status_leds_set_tap_capture(true);
        status_leds_tap_feedback();
        ESP_LOGI(TAG, "Tap capture ON - short presses now tap tempo");
    }
    render_ui();
}

static void toggle_tuner(void)
{
    uint8_t value = s_tuner_on ? 0 : 127;

    if (send_cc(CC_TUNER, value) != ESP_OK) {
        ESP_LOGW(TAG, "Tuner command failed");
        return;
    }

    s_tuner_on = !s_tuner_on;
    s_screen = s_tuner_on ? UI_SCREEN_TUNER : UI_SCREEN_PERFORMANCE;
    ESP_LOGI(TAG, "TUNER -> %s", s_tuner_on ? "ON" : "OFF");
    render_ui();
}

static void toggle_block(controller_block_t block)
{
    uint8_t cc = nanocore_block_onoff_cc(block);
    uint8_t current = 0;

    bool known = false;
    if (block < BLOCK_COUNT && s_block_known[block]) {
        known = true;
        current = s_block_on[block] ? 127 : 0;
    } else {
        known = effective_cc(cc, &current);
    }

    uint8_t next = (!known || current < 64) ? 127 : 0;
    ESP_LOGI(TAG, "STOMP %s -> %s",
             nanocore_block_name(block),
             next >= 64 ? "ON" : "OFF");

    (void)set_working_cc(cc, next);
}

static uint16_t current_parameter_value(bool *known)
{
    if (known) *known = false;

    if (s_selected_block >= BLOCK_COUNT) return 0;

    if (s_selected_parameter == 0) {
        if (!s_block_known[s_selected_block]) return 0;
        if (known) *known = true;
        return s_block_on[s_selected_block] ? 127 : 0;
    }

    if (s_selected_parameter == 1) {
        int index = 0;
        if (!current_selector_index(s_selected_block, &index)) return 0;
        if (known) *known = true;
        return (uint16_t)index;
    }

    return 0;
}

static void select_parameter(int delta)
{
    size_t count = editor_parameter_count();
    if (count == 0) return;

    int p = (int)s_selected_parameter + delta;
    while (p < 0) p += (int)count;
    while (p >= (int)count) p -= (int)count;
    s_selected_parameter = (size_t)p;
    render_ui();
}

static void adjust_parameter(int delta)
{
    if (delta == 0 || s_selected_block >= BLOCK_COUNT) return;

    controller_block_t block = s_selected_block;

    if (s_selected_parameter == 0) {
        uint8_t current =
            (s_block_known[block] && s_block_on[block]) ? 127 : 0;
        uint8_t next = delta > 0 ? 127 : 0;

        if (next != current) {
            (void)set_working_cc(
                nanocore_block_onoff_cc(block), next);
        }
        return;
    }

    if (s_selected_parameter == 1) {
        int current = 0;
        if (!current_selector_index(block, &current)) current = 0;

        const int count =
            (int)nanocore_spec_block_selector_count(block);
        if (count <= 0) return;

        int next = current + delta;
        while (next < 0) next += count;
        while (next >= count) next -= count;

        uint8_t raw = 0;
        if (block == BLOCK_AMP || block == BLOCK_CAB) {
            raw = (uint8_t)next;
        } else {
            const nanocore_effect_type_info_t *info =
                nanocore_spec_block_selector(block,(size_t)next);
            if (!info) return;
            raw = info->id;
        }

        (void)set_working_cc(
            nanocore_block_type_cc(block), raw);
        return;
    }

    uint8_t param_count = 0;
    const nanocore_param_info_t *params =
        current_param_spec(block, &param_count);
    if (!params) return;

    uint8_t param_index =
        (uint8_t)(s_selected_parameter - 2);
    if (param_index >= param_count ||
        param_index >= NC_PROTO_MAX_PARAMS) return;

    float current = 0.0f;
    if (s_block_profile_known[block] &&
        param_index < s_block_param_count[block]) {
        current = s_block_params[block][param_index];
    } else {
        return; /* Never jump from a guessed zero: wait for 0x63 authority. */
    }

    float next = nanocore_spec_adjust_normalized(
        &params[param_index], current, delta);

    if (set_profile_param(block,param_index,next) != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s parameter write failed param=%u",
                 nanocore_block_name(block), param_index);
    }

    render_ui();
}

static void open_block_editor(void)
{
    s_selected_parameter = 0;
    s_parameter_editing = false;
    s_screen = UI_SCREEN_BLOCK_EDITOR;

    /*
     * Every Stage 07 editor is driven by the live 0x63 snapshot. AMP/CAB
     * additionally need the authoritative saved model id from 0x41.
     */
    if (s_midi_ready && !s_recall_pending && !s_recall_poll_inflight) {
        s_editor_models_after_live =
            (s_selected_block == BLOCK_AMP && !s_amp_model_known) ||
            (s_selected_block == BLOCK_CAB && !s_cab_model_known);

        esp_err_t rc = nanocore_protocol_request_live_state();
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "Editor live refresh failed: %s",
                     esp_err_to_name(rc));

            if (s_editor_models_after_live) {
                s_editor_models_after_live = false;
                (void)nanocore_protocol_request_preset_models(
                    s_runtime.current_preset);
            }
        }
    }

    render_ui();
}

static void back_from_block_editor(void)
{
    if (s_parameter_editing) {
        s_parameter_editing = false;
    } else {
        s_screen = UI_SCREEN_PERFORMANCE;
    }
    render_ui();
}

static void open_name_editor(void)
{
    const char *initial;

    s_name_target_scene = s_runtime.mode == CTRL_MODE_SCENE;
    if (s_name_target_scene) {
        initial = s_preset.scene[s_runtime.current_scene].name;
    } else {
        initial = s_preset.name;
    }

    name_editor_open(&s_name_editor, initial);
    s_screen = UI_SCREEN_NAME_EDITOR;
    render_ui();
}

static void cancel_name_editor(void)
{
    name_editor_close(&s_name_editor);
    s_screen = UI_SCREEN_PERFORMANCE;
    render_ui();
}

static void commit_name_and_save(void)
{
    if (!s_name_editor.active) return;

    if (s_name_target_scene) {
        strncpy(s_preset.scene[s_runtime.current_scene].name,
                s_name_editor.text,
                CTRL_NAME_LEN - 1);
        s_preset.scene[s_runtime.current_scene].name[CTRL_NAME_LEN - 1] = '\0';

        s_preset.active_scene = s_runtime.current_scene;

        esp_err_t rc = controller_storage_save_preset(
            s_runtime.current_preset, &s_preset);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "Scene/NVS save failed: %s", esp_err_to_name(rc));
            show_notice("SCENE SAVE FAILED");
            return;
        }

        s_dirty = false;
        name_editor_close(&s_name_editor);
        s_screen = UI_SCREEN_PERFORMANCE;
        (void)persist_runtime();
        show_notice("SCENE SAVED TO CONTROLLER");
        return;
    }

    /*
     * NanoCore's real preset-name write limit is 8 characters.
     * Keep the controller's mirror identical to what the pedal stores.
     */
    char device_name[9] = {0};
    strncpy(device_name, s_name_editor.text, 8);

    if (!s_midi_ready) {
        show_notice("NANOCORE OFFLINE - NOT SAVED");
        return;
    }

    show_notice("SAVING TO NANOCORE...");

    esp_err_t rc = nanocore_protocol_rename_and_save(
        s_runtime.current_preset,
        device_name,
        pdMS_TO_TICKS(900));

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "NanoCore save failed: %s", esp_err_to_name(rc));
        show_notice("NANOCORE SAVE FAILED");
        return;
    }

    strncpy(s_preset.name, device_name, CTRL_NAME_LEN - 1);
    s_preset.name[CTRL_NAME_LEN - 1] = '\0';
    s_preset.active_scene = s_runtime.current_scene;

    rc = controller_storage_save_preset(
        s_runtime.current_preset, &s_preset);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "NanoCore saved but NVS mirror failed: %s",
                 esp_err_to_name(rc));
        show_notice("PEDAL SAVED / NVS ERROR");
        return;
    }

    s_dirty = false;
    name_editor_close(&s_name_editor);
    s_screen = UI_SCREEN_PERFORMANCE;
    (void)persist_runtime();

    s_name_after_live = true;
    (void)nanocore_protocol_request_live_state();

    ESP_LOGI(TAG, "NanoCore + NVS saved P%02u / %s",
             s_runtime.current_preset + 1, s_preset.name);
    show_notice("PRESET SAVED TO NANOCORE");
}

static void render_ui(void)
{
    if (!display_ui_is_ready()) return;

    display_ui_state_t u = {
        .ble_connected = s_ble_connected,
        .midi_ready = s_midi_ready,
        .dirty = s_dirty,
        .tap_capture = tempo_engine_tap_active(),
        .screen = s_screen,
        .mode = s_runtime.mode,
        .preset_index = s_runtime.current_preset,
        .scene_index = s_runtime.current_scene,
        .bpm = s_preset.bpm,
        .subdivision = s_preset.delay_subdivision,
        .preset = &s_preset,
        .selected_block = s_selected_block,
        .selected_parameter = s_selected_parameter,
        .parameter_editing = s_parameter_editing,
        .notice = s_notice,
        .name_editor = &s_name_editor,
    };

    memcpy(u.block_known, s_block_known, sizeof(u.block_known));
    memcpy(u.block_on, s_block_on, sizeof(u.block_on));

    char parameter_text[72] = {0};
    controller_block_t block = s_selected_block;

    uint8_t param_count = 0;
    const nanocore_param_info_t *params =
        current_param_spec(block, &param_count);

    u.editor_override = true;
    u.editor_parameter_count = 2 + param_count;
    u.editor_parameter_labels[0] = "ON/OFF";
    u.editor_parameter_labels[1] =
        (block == BLOCK_AMP || block == BLOCK_CAB)
            ? "MODEL" : "TYPE";

    for (uint8_t i=0;
         i<param_count && (size_t)(i+2)<UI_MAX_EDITOR_PARAMETERS;
         ++i) {
        u.editor_parameter_labels[i+2] = params[i].name;
    }

    if (s_selected_parameter < u.editor_parameter_count) {
        u.selected_parameter_label =
            u.editor_parameter_labels[s_selected_parameter];
    }

    if (s_selected_parameter == 0) {
        u.parameter_known = s_block_known[block];
        u.parameter_value = s_block_on[block] ? 127 : 0;

        if (u.parameter_known) {
            snprintf(parameter_text,sizeof(parameter_text),"%s",
                     s_block_on[block] ? "ON" : "OFF");
            u.parameter_text = parameter_text;
        }
    } else if (s_selected_parameter == 1) {
        int selector = 0;
        u.parameter_known =
            current_selector_index(block,&selector);

        if (u.parameter_known) {
            u.parameter_value = (uint16_t)selector;
            const char *name =
                selector_name(block,(size_t)selector);
            if (name) u.parameter_text = name;
        }
    } else if (params) {
        uint8_t param_index =
            (uint8_t)(s_selected_parameter - 2);

        if (s_block_profile_known[block] &&
            param_index < s_block_param_count[block] &&
            param_index < param_count) {

            const nanocore_param_info_t *p =
                &params[param_index];
            float normalized =
                s_block_params[block][param_index];

            const char *discrete =
                nanocore_spec_discrete_label(p,normalized);

            u.parameter_known = true;

            if (discrete) {
                snprintf(parameter_text,sizeof(parameter_text),
                         "%s",discrete);
            } else {
                float real =
                    nanocore_spec_normalized_to_real(p,normalized);

                if (p->unit && strcmp(p->unit,":1")==0) {
                    snprintf(parameter_text,sizeof(parameter_text),
                             "%.*f:1",p->decimals,(double)real);
                } else if (p->unit && p->unit[0]) {
                    snprintf(parameter_text,sizeof(parameter_text),
                             "%.*f %s",
                             p->decimals,(double)real,p->unit);
                } else {
                    snprintf(parameter_text,sizeof(parameter_text),
                             "%.*f",p->decimals,(double)real);
                }
            }

            u.parameter_text = parameter_text;
        }
    }

    display_ui_render(&u);
}

/* -------------------------------------------------------------------------
 * BLE-MIDI central transport discovery
 * ------------------------------------------------------------------------- */

static bool adv_has_midi_uuid(const uint8_t *adv, uint16_t adv_len)
{
    const uint8_t *p = NULL;
    uint8_t len = 0;

    if (esp_ble_conn_parse_adv_data(adv, adv_len,
                                    ESP_BLE_CONN_ADV_TYPE_UUID128_COMPLETE,
                                    &p, &len) == ESP_OK) {
        for (uint8_t off = 0; off + 16 <= len; off += 16) {
            if (memcmp(&p[off], MIDI_SERVICE_UUID, 16) == 0) return true;
        }
    }

    if (esp_ble_conn_parse_adv_data(adv, adv_len,
                                    ESP_BLE_CONN_ADV_TYPE_UUID128_INCOMP,
                                    &p, &len) == ESP_OK) {
        for (uint8_t off = 0; off + 16 <= len; off += 16) {
            if (memcmp(&p[off], MIDI_SERVICE_UUID, 16) == 0) return true;
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
        adv, adv_len,
        ESP_BLE_CONN_ADV_TYPE_NAME_COMPLETE,
        &name, &name_len);

    if (rc != ESP_OK) {
        rc = esp_ble_conn_parse_adv_data(
            adv, adv_len,
            ESP_BLE_CONN_ADV_TYPE_NAME_SHORT,
            &name, &name_len);
    }

    if (rc == ESP_OK && name && name_len) {
        size_t n = name_len < out_len - 1 ? name_len : out_len - 1;
        memcpy(out, name, n);
        out[n] = '\0';
    }
}

static bool contains_nanocore(const char *name)
{
    if (!name || !name[0]) return false;

    char lower[48] = {0};
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;

    for (size_t i = 0; i < n; ++i)
        lower[i] = (char)tolower((unsigned char)name[i]);

    return strstr(lower, "nanocore") != NULL;
}

static bool scan_cb(const esp_ble_conn_scan_result_t *result, void *arg)
{
    (void)arg;
    if (!result || s_target_found) return false;

    char name[48];
    adv_get_name(result->adv_data, result->adv_data_len,
                 name, sizeof(name));

    bool midi = adv_has_midi_uuid(result->adv_data,
                                  result->adv_data_len);

    if (midi || contains_nanocore(name)) {
        ESP_LOGI(TAG,
                 "NANOCORE candidate: " BLE_CONN_MGR_ADDR_STR
                 " RSSI=%d name=%s MIDI=%s",
                 BLE_CONN_MGR_ADDR_HEX(result->addr),
                 result->rssi,
                 name[0] ? name : "<no-name>",
                 midi ? "YES" : "no");
        s_target_found = true;
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

static void midi_rx_cb(const uint8_t *data, uint16_t len, void *ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "BLE-MIDI packet %u bytes", len);
}

static void midi_evt_cb(uint16_t ts_ms,
                        esp_ble_midi_event_type_t type,
                        const uint8_t *msg,
                        uint16_t len)
{
    if (type == ESP_BLE_MIDI_EVENT_SYSEX_OVERFLOW) {
        ESP_LOGW(TAG, "MIDI RX SysEx overflow");
        return;
    }

    if (!msg || len == 0) return;

    if ((msg[0] & 0xF0) == 0xB0 && len >= 3) {
        ESP_LOGI(TAG, "MIDI RX CC%u=%u", msg[1] & 0x7F, msg[2] & 0x7F);
    } else if (msg[0] == 0xF0) {
        ESP_LOGD(TAG, "MIDI RX SysEx ts=%u len=%u", ts_ms, len);
        nanocore_protocol_on_sysex(msg, len);
    }
}

static void subscribe_midi(uint16_t conn_handle)
{
    static uint8_t cccd[2] = {0x01, 0x00};

    esp_ble_conn_data_t sub = {
        .type = BLE_CONN_UUID_TYPE_128,
        .write_conn_id = conn_handle,
        .data = cccd,
        .data_len = sizeof(cccd),
    };
    memcpy(sub.uuid.uuid128, MIDI_CHAR_UUID, sizeof(sub.uuid.uuid128));

    esp_err_t rc = esp_ble_conn_subscribe_by_handle(
        conn_handle,
        ESP_BLE_CONN_DESC_CIENT_CONFIG,
        &sub);

    ESP_LOGI(TAG, "MIDI notify subscribe: %s", esp_err_to_name(rc));
}

static bool resolve_midi_handle(uint16_t conn_handle)
{
    esp_ble_conn_uuid_t svc = {0};
    esp_ble_conn_uuid_t chr = {0};
    memcpy(svc.uuid128, MIDI_SERVICE_UUID, sizeof(svc.uuid128));
    memcpy(chr.uuid128, MIDI_CHAR_UUID, sizeof(chr.uuid128));

    s_midi_char_handle = 0;

    esp_err_t rc = esp_ble_conn_get_peer_chr_handle(
        conn_handle,
        BLE_CONN_UUID_TYPE_128, svc,
        BLE_CONN_UUID_TYPE_128, chr,
        &s_midi_char_handle);

    if (rc != ESP_OK || !s_midi_char_handle) {
        ESP_LOGE(TAG, "MIDI characteristic not found: %s",
                 esp_err_to_name(rc));
        return false;
    }

    return true;
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
        ESP_LOGI(TAG, "BLE scanning...");
        break;

    case ESP_BLE_CONN_EVENT_CONNECTED:
        if (esp_ble_conn_get_conn_handle(&s_conn_handle) == ESP_OK) {
            s_ble_connected = true;
            ESP_LOGI(TAG, "BLE connected handle=%u", s_conn_handle);
            (void)esp_ble_conn_mtu_update(s_conn_handle, 185);
            render_ui();
        }
        break;

    case ESP_BLE_CONN_EVENT_DISC_COMPLETE:
        if (s_conn_handle != BLE_CONN_HANDLE_INVALID &&
            resolve_midi_handle(s_conn_handle)) {
            subscribe_midi(s_conn_handle);
            nanocore_midi_set_link(s_conn_handle, s_midi_char_handle);

            s_midi_ready = true;
            nanocore_protocol_set_connected(true);
            render_ui();
            restore_current_preset_to_nanocore();
        }
        break;

    case ESP_BLE_CONN_EVENT_DATA_RECEIVE: {
        esp_ble_conn_data_t *d = (esp_ble_conn_data_t *)event_data;
        if (d && d->data && d->data_len && conn_data_is_midi_io(d)) {
            esp_ble_midi_on_bep_received(d->data, d->data_len);
        }
        break;
    }

    case ESP_BLE_CONN_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "BLE disconnected");
        nanocore_protocol_set_connected(false);
        nanocore_midi_clear_link();
        clear_block_state();
        s_ble_connected = false;
        s_midi_ready = false;
        s_tuner_on = false;
        s_editor_models_after_live = false;
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_midi_char_handle = 0;
        s_target_found = false;
        s_screen = UI_SCREEN_PERFORMANCE;
        render_ui();
        (void)esp_ble_conn_scan_start();
        break;

    default:
        break;
    }
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(
        BLE_CONN_MGR_EVENTS,
        ESP_EVENT_ANY_ID,
        ble_event_handler,
        NULL));

    esp_ble_conn_config_t config = {0};
    strncpy((char *)config.device_name,
            "Nanocore_Controller_V2",
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
}

/* -------------------------------------------------------------------------
 * UI / physical input routing
 * ------------------------------------------------------------------------- */

static void handle_name_editor_event(input_event_t e)
{
    switch (e) {
    case INPUT_ENC1_CLICK:
        name_editor_select(&s_name_editor);
        break;
    case INPUT_ENC2_CLICK:
        name_editor_toggle_case(&s_name_editor);
        break;
    case INPUT_FS_TOP_LEFT:
        name_editor_cursor_move(&s_name_editor, -1);
        break;
    case INPUT_FS_TOP_RIGHT:
        name_editor_cursor_move(&s_name_editor, +1);
        break;
    case INPUT_FS_PRESET_UP:
        // Physical bottom #1 after corrected pin map.
        name_editor_backspace(&s_name_editor);
        break;
    case INPUT_FS_PRESET_DOWN:
        // Physical bottom #2 after corrected pin map.
        cancel_name_editor();
        return;
    case INPUT_CHORD_SAVE:
        commit_name_and_save();
        return;
    default:
        return;
    }

    render_ui();
}

static void handle_encoder_delta(uint8_t encoder, int delta)
{
    if (delta == 0) return;

    if (s_screen == UI_SCREEN_NAME_EDITOR) {
        if (encoder == 1) {
            name_editor_move_horizontal(&s_name_editor, delta);
        } else {
            name_editor_move_vertical(&s_name_editor, delta);
        }
        render_ui();
        return;
    }

    if (s_tuner_on) return;

    if (encoder == 1) {
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            if (!s_parameter_editing) {
                select_parameter(delta);
            }
        } else {
            int block = (int)s_selected_block + delta;
            while (block < 0) block += BLOCK_COUNT;
            while (block >= BLOCK_COUNT) block -= BLOCK_COUNT;
            s_selected_block = (controller_block_t)block;
            render_ui();
        }
    } else if (encoder == 2) {
        if (s_screen == UI_SCREEN_BLOCK_EDITOR && s_parameter_editing) {
            adjust_parameter(delta);
        }
    }
}

static void handle_input(input_event_t e)
{
    if (s_screen == UI_SCREEN_NAME_EDITOR) {
        handle_name_editor_event(e);
        return;
    }

    if (s_tuner_on) {
        if (e == INPUT_CHORD_TUNER || e == INPUT_ENC2_CLICK) {
            toggle_tuner();
        }
        return;
    }

    /*
     * Stage 06.2 Live Mapper is capture-on-demand. ENC2 is deliberately
     * intercepted before normal UI Back behavior while mapper mode is active.
     */
    if (nanocore_mapper_is_enabled() && e == INPUT_ENC2_CLICK) {
        controller_block_t target = nanocore_mapper_target();

        if (target == BLOCK_AMP || target == BLOCK_CAB) {
            esp_err_t rc = nanocore_protocol_request_preset_models(
                s_runtime.current_preset);
            if (rc == ESP_OK) {
                show_notice("READ SAVED AMP/CAB");
            } else {
                ESP_LOGW(TAG, "Preset model read failed: %s",
                         esp_err_to_name(rc));
                show_notice("MODEL READ FAILED");
            }
        } else {
            nanocore_mapper_request_capture();
            show_notice("MAPPER CAPTURE");
        }
        return;
    }

    switch (e) {
    case INPUT_ENC1_CLICK:
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            s_parameter_editing = !s_parameter_editing;
            render_ui();
        } else {
            open_block_editor();
        }
        break;

    case INPUT_ENC2_CLICK:
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            back_from_block_editor();
        }
        break;

    case INPUT_FS_PRESET_DOWN:
        // Physical bottom #2: PRESET-/SCENE preset navigation,
        // but dedicated REVERB stomp while in STOMP mode.
        if (s_runtime.mode == CTRL_MODE_STOMP) {
            toggle_block(BLOCK_REVERB);
        } else {
            change_preset(-1);
        }
        break;

    case INPUT_FS_PRESET_UP:
        // Physical bottom #1: PRESET+, dedicated MOD stomp in STOMP.
        if (s_runtime.mode == CTRL_MODE_STOMP) {
            toggle_block(BLOCK_MOD);
        } else {
            change_preset(+1);
        }
        break;

    case INPUT_FS_MODE_SHORT:
        if (tempo_engine_tap_active()) {
            register_tap();
        } else {
            cycle_mode();
        }
        break;

    case INPUT_FS_MODE_LONG:
        toggle_tap_capture();
        break;

    case INPUT_CHORD_TUNER:
        toggle_tuner();
        break;

    case INPUT_CHORD_SAVE:
        open_name_editor();
        break;

    case INPUT_CHORD_MAPPER: {
        bool next = !nanocore_mapper_is_enabled();

        if (next) {
            /*
             * Mapper target is frozen to the currently selected performance
             * block for the whole assisted session.
             */
            nanocore_mapper_set_target(s_selected_block);
        }

        nanocore_mapper_set_enabled(next);

        if (next) {
            char notice[64];
            snprintf(notice, sizeof(notice),
                     "MAP %s - ENC2 CAPTURE",
                     nanocore_block_name(
                         nanocore_mapper_target()));
            show_notice(notice);
        } else {
            show_notice("ASSISTED MAPPER OFF");
        }
        break;
    }

    case INPUT_FS_TOP_LEFT:
        if (s_runtime.mode == CTRL_MODE_SCENE) {
            change_scene(-1);
        } else if (s_runtime.mode == CTRL_MODE_STOMP) {
            toggle_block(BLOCK_FX2);
        }
        break;

    case INPUT_FS_TOP_RIGHT:
        if (s_runtime.mode == CTRL_MODE_SCENE) {
            change_scene(+1);
        } else if (s_runtime.mode == CTRL_MODE_STOMP) {
            toggle_block(BLOCK_DELAY);
        }
        break;

    default:
        break;
    }
}

static void handle_protocol_event(const nanocore_protocol_event_t *e)
{
    if (!e) return;

    switch (e->type) {
    case NC_PROTO_EVENT_LIVE_STATE:
        s_recall_poll_inflight = false;

        if (e->slot >= CTRL_PRESET_COUNT) {
            ESP_LOGW(TAG, "Ignoring invalid LIVE slot=%u", e->slot);
            break;
        }

        if (s_recall_pending) {
            if (e->slot != s_recall_target) {
                /*
                 * Expected during Program Change: a 0x63 request issued
                 * while NanoCore is still switching can return the OLD slot.
                 * Never let that stale reply roll controller state backwards.
                 */
                ESP_LOGW(TAG,
                         "Stale LIVE slot=%u ignored; waiting for target=%u",
                         e->slot, s_recall_target);

                s_recall_next_poll_us =
                    esp_timer_get_time() +
                    (RECALL_RETRY_MS * 1000LL);
                break;
            }

            ESP_LOGI(TAG,
                     "Recall confirmed: LIVE slot=%u after %u poll(s)",
                     e->slot, s_recall_poll_count);
            s_recall_pending = false;
        } else if (e->slot != s_runtime.current_preset) {
            /*
             * A stray/delayed 0x63 must never select a preset by itself.
             * Physical preset changes are handled by a valid slot push,
             * which first updates current_preset and then requests 0x63.
             */
            ESP_LOGW(TAG,
                     "Unexpected LIVE slot=%u while controller=%u; ignored",
                     e->slot, s_runtime.current_preset);
            break;
        }

        memcpy(s_block_known, e->block_known, sizeof(s_block_known));
        memcpy(s_block_on, e->block_on, sizeof(s_block_on));
        memcpy(s_block_profile_known, e->block_profile_known,
               sizeof(s_block_profile_known));
        memcpy(s_block_variant, e->block_variant,
               sizeof(s_block_variant));
        memcpy(s_block_param_count, e->block_param_count,
               sizeof(s_block_param_count));
        memcpy(s_block_params, e->block_params,
               sizeof(s_block_params));

        // Stage 06 mapper consumes the exact accepted LIVE snapshot.
        nanocore_mapper_on_live_state(e);

        render_ui();

        if (s_editor_models_after_live) {
            s_editor_models_after_live = false;
            (void)nanocore_protocol_request_preset_models(
                s_runtime.current_preset);
        }

        if (s_runtime.mode == CTRL_MODE_SCENE) {
            // Only overlay a Scene after the requested physical preset
            // has definitely finished recalling.
            apply_current_scene();
        }

        // Mapper polls must not trigger continuous 0x41/name traffic.
        // Read the name only after recall/save explicitly asked for it.
        if (s_name_after_live) {
            s_name_after_live = false;
            (void)nanocore_protocol_request_preset_name(
                s_runtime.current_preset);
        }
        break;

    case NC_PROTO_EVENT_BLOCK_STATE: {
        if (e->block >= BLOCK_COUNT) break;

        s_block_known[e->block] = true;
        s_block_on[e->block] = e->on;

        /*
         * A physical NanoCore block toggle is a real unsaved live edit.
         * Mirror it in the current controller context so SAVE preserves the
         * same state in NVS as the pedal.
         */
        size_t count = 0;
        midi_assignment_t *items = edit_assignments(&count);
        (void)controller_assignment_set(
            items, count,
            nanocore_block_onoff_cc(e->block),
            e->on ? 127 : 0);
        s_dirty = true;
        render_ui();
        break;
    }

    case NC_PROTO_EVENT_ACTIVE_SLOT:
        /*
         * Current NanoCore firmware is known to emit unreliable 0x01 values
         * around Program Change (including values outside 0..63). During a
         * controller-originated recall, 0x63 is the only slot authority.
         */
        if (s_recall_pending) {
            ESP_LOGI(TAG,
                     "Slot push %u ignored during recall target=%u",
                     e->slot, s_recall_target);
            break;
        }

        if (e->slot < CTRL_PRESET_COUNT &&
            e->slot != s_runtime.current_preset) {
            bool had_dirty = s_dirty;
            ESP_LOGI(TAG, "Physical NanoCore preset -> %u", e->slot);
            (void)load_preset(e->slot, false);
            clear_block_state();

            // The push is a candidate; use the exact same 0x63 confirmation
            // path before accepting live state/name.
            schedule_recall_sync(e->slot);

            show_notice(had_dirty
                ? "NANOCORE CHANGED - LOCAL EDIT LOST"
                : "NANOCORE PRESET CHANGED");
        }
        break;

    case NC_PROTO_EVENT_PRESET_NAME:
        if (e->slot == s_runtime.current_preset &&
            s_screen != UI_SCREEN_NAME_EDITOR &&
            e->name[0]) {
            strncpy(s_preset.name, e->name, CTRL_NAME_LEN - 1);
            s_preset.name[CTRL_NAME_LEN - 1] = '\0';

            // Keep the local mirror's display name from becoming stale
            // (e.g. an old test name such as "Prueba").
            if (!s_dirty) {
                esp_err_t rc = controller_storage_save_preset(
                    s_runtime.current_preset, &s_preset);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG,
                             "Could not refresh NVS preset name: %s",
                             esp_err_to_name(rc));
                }
            }

            render_ui();
        }
        break;

    case NC_PROTO_EVENT_PRESET_MODELS: {
        if (e->slot != s_runtime.current_preset) break;

        const char *amp_name = e->amp_model_known
            ? nanocore_spec_amp_model_name(e->amp_model) : NULL;
        const char *cab_name = e->cab_model_known
            ? nanocore_spec_cab_model_name(e->cab_model) : NULL;

        ESP_LOGI(TAG,
                 "MODEL|slot=%u|AMP=%s(%u)|CAB=%s(%u)",
                 e->slot,
                 amp_name ? amp_name : "UNKNOWN", e->amp_model,
                 cab_name ? cab_name : "UNKNOWN", e->cab_model);

        if (e->amp_model_known) {
            s_amp_model_known = true;
            s_amp_model = e->amp_model;
        }
        if (e->cab_model_known) {
            s_cab_model_known = true;
            s_cab_model = e->cab_model;
        }
        render_ui();

        controller_block_t target = nanocore_mapper_target();
        if (nanocore_mapper_is_enabled() &&
            target == BLOCK_AMP && e->amp_model_known) {
            char notice[64];
            snprintf(notice, sizeof(notice), "AMP %u %s",
                     e->amp_model, amp_name ? amp_name : "?");
            show_notice(notice);
        } else if (nanocore_mapper_is_enabled() &&
                   target == BLOCK_CAB && e->cab_model_known) {
            char notice[64];
            snprintf(notice, sizeof(notice), "CAB %u %s",
                     e->cab_model, cab_name ? cab_name : "?");
            show_notice(notice);
        }
        break;
    }

    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " NANOCORE MIDI CONTROLLER - STAGE 07 / FULL DYNAMIC EDITOR");
    ESP_LOGI(TAG, " ESP32-S3 N16R8 + ST7796S 3.5");
    ESP_LOGI(TAG, "==============================================");

    nvs_init_safe();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(controller_storage_init());

    ESP_ERROR_CHECK(controller_storage_load_runtime(&s_runtime));
    ESP_ERROR_CHECK(controller_storage_load_preset(
        s_runtime.current_preset,
        &s_preset));

    if (s_runtime.current_scene >= CTRL_SCENE_COUNT) {
        s_runtime.current_scene = 0;
    }

    tempo_engine_init(s_preset.bpm);

    // Bring up the display before background input/LED tasks so a fault in
    // those subsystems cannot hide boot diagnostics or leave the TFT dark.
    ESP_ERROR_CHECK(display_ui_init());
    render_ui();

    ESP_ERROR_CHECK(status_leds_init(s_runtime.mode, s_preset.bpm));
    ESP_ERROR_CHECK(input_manager_init());
    ESP_ERROR_CHECK(nanocore_protocol_init());
    ESP_ERROR_CHECK(nanocore_mapper_init());

    ble_init();

    bool last_tap_active = tempo_engine_tap_active();

    while (1) {
        /*
         * Rotation is coalesced: one fast physical turn may arrive as +20
         * detents, which updates the model once and renders once instead of
         * replaying 20 stale screen frames.
         */
        int enc1 = input_manager_take_encoder_delta(1);
        int enc2 = input_manager_take_encoder_delta(2);
        if (enc1) handle_encoder_delta(1, enc1);
        if (enc2) handle_encoder_delta(2, enc2);

        input_event_t e = INPUT_NONE;
        TickType_t input_wait = pdMS_TO_TICKS(10);
        if (input_wait < 1) input_wait = 1;
        if (input_manager_get_event(&e, input_wait)) {
            handle_input(e);
        }

        /*
         * Stage 06 protocol events carry all live float parameters and are
         * intentionally large. Keep the receive buffer out of app_main's
         * task stack as well.
         */
        static nanocore_protocol_event_t pe;
        while (nanocore_protocol_get_event(&pe, 0)) {
            handle_protocol_event(&pe);
        }

        service_recall_sync();
        nanocore_mapper_service(
            s_midi_ready,
            s_recall_pending || s_recall_poll_inflight);
        tempo_engine_service();

        bool tap_active = tempo_engine_tap_active();
        if (tap_active != last_tap_active) {
            status_leds_set_tap_capture(tap_active);
            last_tap_active = tap_active;
            render_ui();
        }

        if (s_notice[0] &&
            esp_timer_get_time() >= s_notice_until_us) {
            s_notice[0] = '\0';
            render_ui();
        }

        // Always give IDLE0 CPU time. On a 100 Hz FreeRTOS tick,
        // pdMS_TO_TICKS(5) was zero and the previous loop could spin forever,
        // tripping the task watchdog even while the controller appeared alive.
        vTaskDelay(1);
    }
}
