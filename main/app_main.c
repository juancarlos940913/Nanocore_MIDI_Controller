/*
 * Nanocore MIDI Controller - Stage 07.1 / Dynamic AMP/CAB Catalog
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
#include <math.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_conn_mgr.h"
#include "esp_ble_midi.h"
#include "esp_ble_midi_svc.h"

#include "board.h"
#include "battery_manager.h"
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

/* Stage 07.1: one bounded background catalog sweep per BLE connection. */
#define CATALOG_START_MS       2000
#define CATALOG_IDLE_MS         950
#define CATALOG_EVENT_GAP_MS    300
#define CATALOG_GAP_MS           35
#define CATALOG_TIMEOUT_MS      900
#define CATALOG_AMP_TIMEOUT_MS 2500
#define CATALOG_AMP_INFO_GAP_MS 3000
#define CATALOG_MAX_FAILURES      3
#define CATALOG_TOTAL          (2 * NC_PROTO_MODEL_COUNT)
#define SYNMAP_TIMEOUT_MS 3000

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
static bool s_editor_models_after_live;

/* Only app_main advances the sweep. The BLE task publishes link epochs
 * and can render the UI, so protect the tiny cache/epoch copies. */
static portMUX_TYPE s_catalog_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_catalog_link_epoch;
static uint32_t s_catalog_cache_epoch;
static bool s_catalog_link_active;
static char s_catalog_names[2][NC_PROTO_MODEL_COUNT][NC_PROTO_MODEL_NAME_LEN + 1];
/* 0 = unknown/unread, 1 = real name, 2 = empty slot. */
static uint8_t s_catalog_state[2][NC_PROTO_MODEL_COUNT];
static bool s_catalog_inflight;
static bool s_synmap_enabled;
static bool s_synmap_inflight;
static uint16_t s_synmap_seq;
static int64_t s_synmap_sent_us;
static bool s_catalog_amp_info_sent;
static bool s_catalog_cab_info_sent;
static int s_pending_scene_delta;
static uint8_t s_models_read_retries;
static uint16_t s_catalog_seq;
static uint8_t s_catalog_cursor;
static uint8_t s_catalog_retry;
static uint8_t s_catalog_failures;
static uint8_t s_catalog_read_count;
static uint8_t s_catalog_empty_count;
static int64_t s_catalog_sent_us;
static int64_t s_catalog_next_us;
static int64_t s_catalog_quiet_until_us;

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
/* Stage 08D read-only preview: keep Synth outside legacy block arrays. */
static bool s_synth_mode_known;
static bool s_synth_mode;
static bool s_synth_selected;
static bool s_levels_selected, s_levels_known, s_levels_wait, s_levels_edit;
static bool s_levels_write_ready, s_levels_write_pending;
static int8_t s_levels_target_db;
static uint8_t s_levels_row, s_level_raw;
static bool s_level_raw_known;
static int8_t s_input_gain_db;
static uint16_t s_levels_seq;
static int64_t s_levels_deadline;
static bool s_slot_probe_pending, s_slot_probe_inflight;
static uint16_t s_slot_probe_seq;
static int64_t s_slot_probe_due, s_slot_probe_deadline;
static bool s_synth_voice_known;
static bool s_synth_voice_on;
static uint8_t s_synth_voice;
static uint8_t s_synth_param_count;
static float s_synth_params[NC_PROTO_SYNTH_MAX_PARAMS];
static const controller_block_t s_synth_visible[] = {
    BLOCK_FX1, BLOCK_FX1, BLOCK_FX2, BLOCK_DELAY, BLOCK_MOD, BLOCK_REVERB, BLOCK_EQ
};
static size_t s_selected_parameter;
static bool s_parameter_editing;

static ui_screen_t s_screen = UI_SCREEN_PERFORMANCE;

static name_editor_t s_name_editor;
static bool s_name_target_scene;

/* Main-task scene transaction. Legacy preset blobs remain unchanged. */
typedef enum {
    SCENE_IDLE = 0, SCENE_MODE_EDIT, SCENE_MODE_RECALL,
    SCENE_VERIFY, SCENE_SAVE, SCENE_BASE_RESTORE, SCENE_BASE_SAVE
} scene_phase_t;
static scene_phase_t s_scene_phase;
static bool s_scene_after_models;
static int64_t s_scene_base_deadline;
static uint16_t s_scene_seq;
static int64_t s_scene_deadline;
static bool s_scene_target_mode;
static uint8_t s_scene_read_retries;
static bool s_scene_fault;
static controller_scene_profile_t s_scene_profile;
static controller_scene_profile_t s_scene_name_scratch;
static bool s_scene_saved[CTRL_SCENE_COUNT];
static bool s_base_mode_known;
static bool s_base_mode;
static bool s_scene_verified_models;
static bool s_scene_force_models;
static int64_t s_scene_started_us;

static bool scene_busy(void)
{
    return s_scene_phase != SCENE_IDLE || s_scene_after_models;
}
static void apply_current_scene(void);
static void scene_select_mode(bool synth);
static void scene_request_save(void);
static void commit_base_save(void);

static void render_ui(void);

/* Keep a mode-only round trip distinct from real parameter edits.
 * All snapshots are RAM-only, never persisted implicitly. */
typedef enum {
    DISCARD_NONE=0, DISCARD_PRESET_DOWN, DISCARD_PRESET_UP,
    DISCARD_SCENE_DOWN, DISCARD_SCENE_UP, DISCARD_MODE
} discard_action_t;
static discard_action_t s_discard_action;
static bool s_sound_origin_valid;
static controller_scene_profile_t s_sound_origin;

static void clear_edit_dirty(void)
{
    s_dirty=false;
    s_sound_origin_valid=false;
    s_discard_action=DISCARD_NONE;
}

static void mark_edit_dirty(void)
{
    s_dirty=true;
    s_sound_origin_valid=false;
}

static void sound_edit_begin(void)
{
    if (s_dirty || s_sound_origin_valid || !s_synth_mode_known ||
        !s_synth_voice_known || s_synth_param_count>CTRL_PROFILE_SYNTH_PARAMS) return;
    controller_scene_profile_t *p=&s_sound_origin;
    memset(p,0,sizeof(*p));
    p->synth_mode=s_synth_mode;
    p->synth_on=s_synth_voice_on;
    p->synth_voice=s_synth_voice;
    p->synth_count=s_synth_param_count;
    memcpy(p->synth_params,s_synth_params,sizeof(p->synth_params));
    for (int b=0;b<BLOCK_COUNT;++b) {
        if (!s_block_known[b] || !s_block_profile_known[b] ||
            s_block_param_count[b]>CTRL_PROFILE_PARAMS) return;
        p->block[b].on=s_block_on[b];
        p->block[b].type=s_block_variant[b]; /* 0x63 raw type, not model ID */
        p->block[b].count=s_block_param_count[b];
        memcpy(p->block[b].params,s_block_params[b],sizeof(p->block[b].params));
    }
    s_sound_origin_valid=true;
}

static bool sound_vector_equal(const float *a,const float *b,uint8_t count)
{
    for(uint8_t i=0;i<count;++i)
        if(!(fabsf(a[i]-b[i])<=0.00001f)) return false;
    return true;
}

static void sound_edit_confirm(const nanocore_protocol_event_t *e)
{
    s_dirty=true;
    if(!s_sound_origin_valid || e->synth_mode!=s_sound_origin.synth_mode) return;
    const controller_scene_profile_t *p=&s_sound_origin;
    bool same=e->synth_known && e->synth_on==p->synth_on &&
        e->synth_variant==p->synth_voice && e->synth_param_count==p->synth_count &&
        sound_vector_equal(e->synth_params,p->synth_params,p->synth_count);
    for(int b=0;same && b<BLOCK_COUNT;++b) {
        const controller_scene_block_t *r=&p->block[b];
        same=e->block_known[b] && e->block_profile_known[b] &&
            e->block_on[b]==r->on && e->block_variant[b]==r->type &&
            e->block_param_count[b]==r->count &&
            sound_vector_equal(e->block_params[b],r->params,r->count);
    }
    s_sound_origin_valid=false;
    if(same) {
        clear_edit_dirty();
        ESP_LOGI(TAG,"EDIT|ROUNDTRIP_CLEAN|mode=%u",e->synth_mode);
    } else {
        ESP_LOGI(TAG,"EDIT|ROUNDTRIP_CHANGED|mode=%u",e->synth_mode);
    }
}

static const char *discard_message(void)
{
    switch(s_discard_action) {
    case DISCARD_PRESET_DOWN: return "UNSAVED - PRESS PRESET- AGAIN TO DISCARD";
    case DISCARD_PRESET_UP: return "UNSAVED - PRESS PRESET+ AGAIN TO DISCARD";
    case DISCARD_SCENE_DOWN: return "UNSAVED - PRESS SCENE- AGAIN TO DISCARD";
    case DISCARD_SCENE_UP: return "UNSAVED - PRESS SCENE+ AGAIN TO DISCARD";
    case DISCARD_MODE: return "UNSAVED - PRESS MODE AGAIN TO DISCARD";
    default: return NULL;
    }
}

static void discard_cancel(void)
{
    if(s_discard_action==DISCARD_NONE) return;
    s_discard_action=DISCARD_NONE;
    s_notice[0]='\0';
    ESP_LOGI(TAG,"EDIT|DISCARD_CANCEL");
}

static bool discard_confirm(discard_action_t action)
{
    if(!s_dirty) {
        discard_cancel();
        return true;
    }
    if(s_discard_action!=action) {
        s_discard_action=action;
        ESP_LOGI(TAG,"EDIT|DISCARD_ARM|action=%u",(unsigned)action);
        render_ui();
        return false;
    }
    s_discard_action=DISCARD_NONE;
    s_notice[0]='\0';
    ESP_LOGI(TAG,"EDIT|DISCARD_CONFIRMED|action=%u",(unsigned)action);
    return true;
}

static void queue_notice(const char *text)
{
    if (!text) return;
    strncpy(s_notice, text, sizeof(s_notice) - 1);
    s_notice[sizeof(s_notice) - 1] = '\0';
    s_notice_until_us = esp_timer_get_time() + (NOTICE_MS * 1000LL);
}

static void show_notice(const char *text)
{
    queue_notice(text);
    render_ui();
}

static void clear_block_state(void)
{
    /* Keep last mode as a write lock until the next authoritative read. */
    s_scene_verified_models = false;
    s_level_raw_known=false;
    s_levels_selected=false;
    s_sound_origin_valid = false;
    s_discard_action = DISCARD_NONE;
    s_synth_mode_known = false;
    s_synth_voice_known = false;
    s_synth_voice_on = false;
    s_synth_param_count = 0;
    memset(s_synth_params, 0, sizeof(s_synth_params));
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
    s_editor_models_after_live = false;
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
    s_slot_probe_pending=false;
    s_slot_probe_inflight=false;
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

/* 0x01 hints never mutate the active preset. Coalesce them and only
 * adopt the slot from a fresh matching, complete 0x63 response. */
static void slot_probe_request(bool immediate)
{
    if(!s_midi_ready || s_recall_pending || scene_busy()) return;
    /* A burst of 0x01 volume hints is not a burst of preset changes.
     * Read once after the last hint; manual ENC2 can request immediately. */
    s_slot_probe_pending=true;
    s_slot_probe_due=esp_timer_get_time()+(immediate?0:200000LL);
}

static void service_slot_probe(void)
{
    if(!s_midi_ready) {
        s_slot_probe_pending=false;
        s_slot_probe_inflight=false;
        s_levels_wait=false;
        s_levels_known=false;
        s_levels_write_ready=false;
        s_levels_write_pending=false;
        s_levels_edit=false;
        s_level_raw_known=false;
        return;
    }
    int64_t now=esp_timer_get_time();
    if(s_slot_probe_inflight && now>=s_slot_probe_deadline) {
        s_slot_probe_inflight=false;
        ESP_LOGW(TAG,"SLOT|VERIFY_TIMEOUT");
    }
    if(s_levels_wait && now>=s_levels_deadline) {
        s_levels_wait=false;
        s_levels_known=false;
        s_levels_write_ready=false;
        s_levels_write_pending=false;
        s_levels_edit=false;
        if(s_screen==UI_SCREEN_LEVELS) show_notice("LEVEL READBACK TIMEOUT");
    }
    if(!s_slot_probe_pending || s_slot_probe_inflight || now<s_slot_probe_due ||
       scene_busy() || s_recall_pending || s_recall_poll_inflight ||
       s_catalog_inflight || s_synmap_enabled || s_levels_wait ||
       nanocore_mapper_is_enabled()) return;
    s_slot_probe_pending=false;
    if(nanocore_protocol_request_live_state_seq(&s_slot_probe_seq)==ESP_OK) {
        s_slot_probe_inflight=true;
        s_slot_probe_deadline=now+3000000LL;
        ESP_LOGI(TAG,"SLOT|VERIFY_TX|seq=%u",s_slot_probe_seq);
    }
}

static void levels_read(void)
{
    if(!s_midi_ready || scene_busy() || s_recall_pending ||
       s_recall_poll_inflight || s_slot_probe_inflight || s_catalog_inflight ||
       s_levels_wait) {
        show_notice("WAIT LIVE SYNC");
        return;
    }
    s_levels_known=false;
    s_levels_write_ready=false;
    s_levels_write_pending=false;
    s_levels_edit=false;
    if(nanocore_protocol_request_global_levels(&s_levels_seq)==ESP_OK) {
        s_levels_wait=true;
        s_levels_deadline=esp_timer_get_time()+2500000LL;
        slot_probe_request(true);
    } else show_notice("LEVEL READ FAILED");
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
        s_recall_poll_inflight = false;
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

static bool current_type_raw(controller_block_t block, uint8_t *raw)
{
    if (block >= BLOCK_COUNT || !raw) return false;

    if (block == BLOCK_AMP) {
        if (s_amp_model_known) {
            *raw = s_amp_model;
            return true;
        }
    } else if (block == BLOCK_CAB) {
        if (s_cab_model_known) {
            *raw = s_cab_model;
            return true;
        }
    } else if (s_block_profile_known[block]) {
        *raw = s_block_variant[block];
        return true;
    }

    /* Last-resort local mirror while a fresh device read is pending. */
    return effective_cc(nanocore_block_type_cc(block), raw);
}

static void catalog_link_changed(bool active)
{
    portENTER_CRITICAL(&s_catalog_lock);
    s_catalog_link_active = active;
    ++s_catalog_link_epoch;
    portEXIT_CRITICAL(&s_catalog_lock);
}

static bool catalog_link_current(void)
{
    portENTER_CRITICAL(&s_catalog_lock);
    bool current = s_catalog_link_active &&
                   s_catalog_link_epoch == s_catalog_cache_epoch;
    portEXIT_CRITICAL(&s_catalog_lock);
    return current;
}

/* Persist only a complete, confirmed sweep. Cache is explicitly marked
 * provisional until this connection re-reads each model. */
typedef struct {
    uint32_t version;
    char names[2][NC_PROTO_MODEL_COUNT][NC_PROTO_MODEL_NAME_LEN + 1];
    uint8_t state[2][NC_PROTO_MODEL_COUNT];
} catalog_cache_t;
static catalog_cache_t s_catalog_disk;

static void catalog_load_cache(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("scene_data","nc_scene1",NVS_READONLY,&h)!=ESP_OK)
        return;
    size_t len=sizeof(s_catalog_disk);
    esp_err_t rc=nvs_get_blob(h,"model_names",&s_catalog_disk,&len);
    nvs_close(h);
    if(rc!=ESP_OK || len!=sizeof(s_catalog_disk) || s_catalog_disk.version!=1) return;
    portENTER_CRITICAL(&s_catalog_lock);
    if(s_catalog_link_active && s_catalog_link_epoch==s_catalog_cache_epoch) {
        for(unsigned g=0;g<2;++g) {
            for(unsigned i=0;i<NC_PROTO_MODEL_COUNT;++i) {
                uint8_t st=s_catalog_disk.state[g][i];
                if(st!=1 && st!=2) continue;
                memcpy(s_catalog_names[g][i],s_catalog_disk.names[g][i],
                       sizeof(s_catalog_names[g][i]));
                s_catalog_names[g][i][NC_PROTO_MODEL_NAME_LEN]='\0';
                s_catalog_state[g][i]=(uint8_t)(st+2);
            }
        }
    }
    portEXIT_CRITICAL(&s_catalog_lock);
    ESP_LOGI(TAG,"CATALOG|CACHE_LOADED|provisional");
}

static void catalog_store_cache(void)
{
    if(s_catalog_read_count != CATALOG_TOTAL) return;
    portENTER_CRITICAL(&s_catalog_lock);
    bool valid=s_catalog_link_active && s_catalog_link_epoch==s_catalog_cache_epoch;
    if(valid) {
        s_catalog_disk.version=1;
        memcpy(s_catalog_disk.names,s_catalog_names,sizeof(s_catalog_disk.names));
        memcpy(s_catalog_disk.state,s_catalog_state,sizeof(s_catalog_disk.state));
    }
    portEXIT_CRITICAL(&s_catalog_lock);
    if(!valid) return;
    nvs_handle_t h;
    esp_err_t rc=nvs_open_from_partition("scene_data","nc_scene1",NVS_READWRITE,&h);
    if(rc==ESP_OK) {
        rc=nvs_set_blob(h,"model_names",&s_catalog_disk,sizeof(s_catalog_disk));
        if(rc==ESP_OK) rc=nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG,"CATALOG|CACHE_SAVED|%s",esp_err_to_name(rc));
}

static void catalog_reset_if_needed(void)
{
    bool changed = false;
    bool active;
    portENTER_CRITICAL(&s_catalog_lock);
    active = s_catalog_link_active;
    if (s_catalog_cache_epoch != s_catalog_link_epoch) {
        memset(s_catalog_names, 0, sizeof(s_catalog_names));
        memset(s_catalog_state, 0, sizeof(s_catalog_state));
        s_catalog_cache_epoch = s_catalog_link_epoch;
        changed = true;
    }
    portEXIT_CRITICAL(&s_catalog_lock);
    if (!changed) return;

    s_catalog_inflight = false;
    s_catalog_amp_info_sent = false;
    s_catalog_cab_info_sent = false;
    s_pending_scene_delta = 0;
    s_catalog_cursor = 0;
    s_catalog_retry = 0;
    s_catalog_failures = 0;
    s_catalog_read_count = 0;
    s_catalog_empty_count = 0;
    s_catalog_sent_us = 0;
    s_catalog_quiet_until_us = 0;
    s_catalog_next_us = esp_timer_get_time() + CATALOG_START_MS * 1000LL;
    if (active) {
        catalog_load_cache();
        ESP_LOGI(TAG, "CATALOG|START|AMP=30|CAB=30");
    }
}

static void catalog_defer(unsigned ms)
{
    int64_t until = esp_timer_get_time() + ms * 1000LL;
    if (until > s_catalog_quiet_until_us) {
        s_catalog_quiet_until_us = until;
    }
}

static void format_model_name(controller_block_t block, uint8_t id,
                              char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (id >= NC_PROTO_MODEL_COUNT ||
        (block != BLOCK_AMP && block != BLOCK_CAB)) {
        snprintf(out, out_len, "UNKNOWN");
        return;
    }

    unsigned group = block == BLOCK_AMP ? 0 : 1;
    uint8_t state = 0;
    char name[NC_PROTO_MODEL_NAME_LEN + 1] = {0};
    portENTER_CRITICAL(&s_catalog_lock);
    if (s_catalog_link_active && s_catalog_link_epoch == s_catalog_cache_epoch) {
        state = s_catalog_state[group][id];
        memcpy(name, s_catalog_names[group][id], sizeof(name));
    }
    portEXIT_CRITICAL(&s_catalog_lock);

    if (state == 1 && name[0]) {
        snprintf(out, out_len, "%s", name);
    } else if (state == 2) {
        snprintf(out, out_len, "SLOT %02u EMPTY", id);
    } else if (state == 3 && name[0]) {
        snprintf(out, out_len, "%s ~", name);
    } else if (state == 4) {
        snprintf(out, out_len, "SLOT %02u EMPTY ~", id);
    } else {
        const char *factory = block == BLOCK_AMP
            ? nanocore_spec_amp_model_name(id)
            : nanocore_spec_cab_model_name(id);
        /* '?' is a deliberate indication that this is not a device read. */
        snprintf(out, out_len, "%s ?", factory ? factory : "UNKNOWN");
    }
}

static controller_block_t catalog_current_block(void)
{
    return s_catalog_cursor < NC_PROTO_MODEL_COUNT ? BLOCK_AMP : BLOCK_CAB;
}

static uint8_t catalog_current_id(void)
{
    return (uint8_t)(s_catalog_cursor % NC_PROTO_MODEL_COUNT);
}

static void catalog_advance(bool success)
{
    s_catalog_inflight = false;
    s_catalog_retry = 0;
    ++s_catalog_cursor;

    if (success) {
        s_catalog_failures = 0;
    } else {
        ++s_catalog_failures;
        /* Do not hammer a firmware/transport that rejects this opcode. */
        if (s_catalog_failures >= CATALOG_MAX_FAILURES &&
            s_catalog_cursor % NC_PROTO_MODEL_COUNT != 0) {
            ESP_LOGW(TAG, "CATALOG|SKIP|remaining %s records after %u failures",
                     s_catalog_cursor < NC_PROTO_MODEL_COUNT ? "AMP" : "CAB",
                     s_catalog_failures);
            s_catalog_cursor = s_catalog_cursor < NC_PROTO_MODEL_COUNT
                ? NC_PROTO_MODEL_COUNT : CATALOG_TOTAL;
        }
    }
    if (s_catalog_cursor % NC_PROTO_MODEL_COUNT == 0) {
        s_catalog_failures = 0;
    }
    s_catalog_next_us = esp_timer_get_time() + CATALOG_GAP_MS * 1000LL;
    if (s_catalog_cursor >= CATALOG_TOTAL) {
        ESP_LOGI(TAG, "CATALOG|DONE|read=%u/%u|empty=%u|unread=%u",
                 s_catalog_read_count, (unsigned)CATALOG_TOTAL,
                 s_catalog_empty_count,
                 (unsigned)(CATALOG_TOTAL - s_catalog_read_count));
        catalog_store_cache();
    }
}

static void catalog_failed(void)
{
    s_catalog_inflight = false;
    if (s_catalog_retry == 0) {
        s_catalog_retry = 1;
        s_catalog_next_us = esp_timer_get_time() + CATALOG_EVENT_GAP_MS * 1000LL;
    } else {
        catalog_advance(false);
    }
}

static void service_model_catalog(void)
{
    if (!s_midi_ready || !catalog_link_current() ||
        s_synmap_enabled || s_catalog_cursor >= CATALOG_TOTAL) return;
    int64_t now = esp_timer_get_time();

    if (s_catalog_inflight) {
        unsigned timeout_ms = catalog_current_block() == BLOCK_AMP
            ? CATALOG_AMP_TIMEOUT_MS : CATALOG_TIMEOUT_MS;
        if (now - s_catalog_sent_us >= timeout_ms * 1000LL) {
            ESP_LOGW(TAG, "CATALOG|TIMEOUT|%s|slot=%u|seq=%u",
                     catalog_current_block() == BLOCK_AMP ? "AMP" : "CAB",
                     catalog_current_id(), s_catalog_seq);
            catalog_failed();
        }
        return;
    }

    /* User input, preset recall and mapper capture take precedence. */
    if (s_recall_pending || s_recall_poll_inflight || scene_busy() ||
        s_slot_probe_pending || s_slot_probe_inflight || s_levels_wait ||
        s_screen==UI_SCREEN_LEVELS ||
        s_tuner_on || s_screen == UI_SCREEN_NAME_EDITOR ||
        nanocore_mapper_is_enabled() ||
        now < s_catalog_next_us || now < s_catalog_quiet_until_us) {
        return;
    }

    /* ToneCommand's USB captures include storage-info before AMP reads.
     * Try this once per connection, with a bounded quiet interval, whether
     * or not this particular firmware answers the information request. */
    if (!s_catalog_amp_info_sent && catalog_current_block() == BLOCK_AMP) {
        s_catalog_amp_info_sent = true;
        esp_err_t info_rc = nanocore_protocol_request_amp_storage_info();
        ESP_LOGI(TAG, "CATALOG|AMP_INFO_TX|op=0x30|%s",
                 esp_err_to_name(info_rc));
        s_catalog_next_us = esp_timer_get_time() +
                            CATALOG_AMP_INFO_GAP_MS * 1000LL;
        return;
    }

    if (!s_catalog_cab_info_sent && catalog_current_block() == BLOCK_CAB) {
        s_catalog_cab_info_sent=true;
        esp_err_t info_rc=nanocore_protocol_request_cab_storage_info();
        ESP_LOGI(TAG,"CATALOG|CAB_INFO_TX|op=0x50|%s",esp_err_to_name(info_rc));
        s_catalog_next_us=esp_timer_get_time()+300000LL;
        return;
    }
    esp_err_t rc = nanocore_protocol_request_model_catalog(
        catalog_current_block(), catalog_current_id(), &s_catalog_seq);
    if (rc == ESP_OK) {
        s_catalog_inflight = true;
        s_catalog_sent_us = now;
    } else {
        ESP_LOGW(TAG, "CATALOG|TX_FAILED|%s|slot=%u|%s",
                 catalog_current_block() == BLOCK_AMP ? "AMP" : "CAB",
                 catalog_current_id(), esp_err_to_name(rc));
        catalog_failed();
    }
}

static void handle_catalog_event(const nanocore_protocol_event_t *e)
{
    if (!s_midi_ready || !s_catalog_inflight ||
        s_catalog_cursor >= CATALOG_TOTAL ||
        e->catalog_seq != s_catalog_seq ||
        e->block != catalog_current_block()) return;

    uint8_t id = catalog_current_id();
    if (e->catalog_status != 0) {
        ESP_LOGW(TAG, "CATALOG|REJECTED|%s|slot=%u|status=%u",
                 e->block == BLOCK_AMP ? "AMP" : "CAB", id, e->catalog_status);
        catalog_advance(false);
        return;
    }
    if (e->slot != id) {
        /* A stale or mismatched record must never label a different slot. */
        return;
    }
    if (e->model_present && !e->model_name[0]) {
        ESP_LOGW(TAG, "CATALOG|EMPTY_NAME|%s|slot=%u",
                 e->block == BLOCK_AMP ? "AMP" : "CAB", id);
        catalog_failed();
        return;
    }

    unsigned group = e->block == BLOCK_AMP ? 0 : 1;
    bool accepted = false;
    portENTER_CRITICAL(&s_catalog_lock);
    if (s_catalog_link_active && s_catalog_link_epoch == s_catalog_cache_epoch) {
        s_catalog_state[group][id] = e->model_present ? 1 : 2;
        memcpy(s_catalog_names[group][id], e->model_name,
               sizeof(s_catalog_names[group][id]));
        accepted = true;
    }
    portEXIT_CRITICAL(&s_catalog_lock);
    if (!accepted) return;

    ++s_catalog_read_count;
    if (!e->model_present) ++s_catalog_empty_count;
    ESP_LOGI(TAG, "CATALOG|%s|slot=%u|name=%s",
             e->block == BLOCK_AMP ? "AMP" : "CAB", id,
             e->model_present ? e->model_name : "<EMPTY>");
    catalog_advance(true);

    uint8_t selected_id;
    if (s_screen == UI_SCREEN_BLOCK_EDITOR && s_selected_parameter == 1 &&
        s_selected_block == e->block &&
        current_type_raw(e->block, &selected_id) && selected_id == id) {
        render_ui();
    }
}

static const nanocore_effect_type_info_t *current_type_info(
    controller_block_t block)
{
    uint8_t raw = 0;
    if (!current_type_raw(block, &raw)) return NULL;
    if (s_synth_mode &&
        nanocore_spec_mode_selector_index(block, true, raw) < 0) return NULL;
    return nanocore_spec_editor_type_by_raw(block, raw);
}

static int current_type_selector(controller_block_t block)
{
    uint8_t raw = 0;
    if (!current_type_raw(block, &raw)) return -1;
    return nanocore_spec_mode_selector_index(block, s_synth_mode, raw);
}

static size_t editor_parameter_count(void)
{
    if (s_synth_selected) {
        const nanocore_effect_type_info_t *voice =
            s_synth_voice_known ? nanocore_spec_synth_voice(s_synth_voice) : NULL;
        return 2 + (voice ? voice->expected_param_count : 0);
    }
    const nanocore_effect_type_info_t *info =
        current_type_info(s_selected_block);
    size_t count = 2 + (info ? info->expected_param_count : 0);

    /* Preserve Stage-05 tempo utilities after the real Delay parameters. */
    if (s_selected_block == BLOCK_DELAY && info) {
        count += 2;  // TEMPO BPM, NOTE
    }

    return count;
}

static esp_err_t set_profile_param(controller_block_t block,
                                   uint8_t param_index,
                                   float normalized)
{
    const nanocore_effect_type_info_t *info =
        current_type_info(block);

    if (!info || !info->params ||
        param_index >= info->expected_param_count ||
        param_index >= NC_PROTO_MAX_PARAMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    esp_err_t rc = nanocore_protocol_set_block_param(
        block, param_index, normalized);

    if (rc == ESP_OK) {
        s_block_profile_known[block] = true;
        s_block_param_count[block] = info->expected_param_count;
        s_block_params[block][param_index] = normalized;
        mark_edit_dirty();

        char value[48];
        nanocore_spec_format_normalized(
            &info->params[param_index], normalized,
            value, sizeof(value));

        char model_text[48];
        const char *type_name = info->name;
        if (nanocore_spec_editor_is_model_block(block)) {
            format_model_name(block, info->id, model_text, sizeof(model_text));
            type_name = model_text;
        }
        ESP_LOGI(TAG,
                 "%s %s %s -> norm=%.6f value=%s",
                 nanocore_block_name(block),
                 type_name,
                 info->params[param_index].name,
                 (double)normalized,
                 value);
    }

    return rc;
}

static esp_err_t send_editor_type_raw(controller_block_t block,
                                      uint8_t raw_type)
{
    const nanocore_effect_type_info_t *info =
        nanocore_spec_editor_type_by_raw(block, raw_type);
    if (!info) return ESP_ERR_INVALID_ARG;

    esp_err_t rc;

    if (nanocore_spec_editor_is_model_block(block)) {
        rc = nanocore_protocol_set_block_model(block, raw_type);
        if (rc == ESP_OK) {
            if (block == BLOCK_AMP) {
                s_amp_model = raw_type;
                s_amp_model_known = true;
            } else {
                s_cab_model = raw_type;
                s_cab_model_known = true;
            }
        }
    } else {
        float defaults[NC_PROTO_MAX_PARAMS] = {0};
        if (!nanocore_spec_editor_defaults(
                info, defaults, NC_PROTO_MAX_PARAMS)) {
            return ESP_ERR_INVALID_ARG;
        }

        rc = nanocore_protocol_set_block_type(
            block, raw_type, defaults, info->expected_param_count);

        if (rc == ESP_OK) {
            s_block_profile_known[block] = true;
            s_block_variant[block] = raw_type;
            s_block_param_count[block] = info->expected_param_count;
            memset(s_block_params[block], 0,
                   sizeof(s_block_params[block]));
            memcpy(s_block_params[block], defaults,
                   (size_t)info->expected_param_count * sizeof(float));
        }
    }

    if (rc == ESP_OK) {
        char model_text[48];
        const char *type_name = info->name;
        if (nanocore_spec_editor_is_model_block(block)) {
            format_model_name(block, raw_type, model_text, sizeof(model_text));
            type_name = model_text;
        }
        ESP_LOGI(TAG,
                 "%s %s -> %s (raw=%u params=%u)",
                 nanocore_block_name(block),
                 nanocore_spec_editor_is_model_block(block)
                     ? "MODEL" : "TYPE",
                 type_name, raw_type, info->expected_param_count);
    }

    return rc;
}

static esp_err_t set_working_cc(uint8_t cc, uint8_t value)
{
    /* A full scene is saved from confirmed LIVE state, not a 16-CC
     * approximation. Do not consume or overwrite legacy sparse overlays. */
    if (s_runtime.mode == CTRL_MODE_SCENE || s_synth_mode) {
        if (!s_midi_ready || !s_synth_mode_known || scene_busy() || s_scene_fault)
            return ESP_ERR_INVALID_STATE;
        int type_block = block_from_type_cc(cc);
        int on_block = block_from_onoff_cc(cc);
        esp_err_t tx;
        if (type_block >= 0) {
            if (nanocore_spec_mode_selector_index((controller_block_t)type_block,
                                                   s_synth_mode, value) < 0)
                return ESP_ERR_INVALID_ARG;
            tx = send_editor_type_raw((controller_block_t)type_block, value);
        } else {
            if (s_synth_mode && (on_block == BLOCK_AMP || on_block == BLOCK_CAB))
                return ESP_ERR_INVALID_ARG;
            tx = send_cc(cc, value);
        }
        if (tx == ESP_OK) {
            if (on_block >= 0) {
                s_block_known[on_block] = true;
                s_block_on[on_block] = value >= 64;
            }
            mark_edit_dirty();
        } else {
            show_notice("SCENE WRITE FAILED");
        }
        render_ui();
        return tx;
    }
    size_t count = 0;
    midi_assignment_t *items = edit_assignments(&count);

    esp_err_t rc = controller_assignment_set(items, count, cc, value);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "No room for CC%u in working context", cc);
        return rc;
    }

    mark_edit_dirty();

    int block = block_from_onoff_cc(cc);
    if (block >= 0) {
        s_block_known[block] = true;
        s_block_on[block] = value >= 64;
    }

    if (s_midi_ready) {
        esp_err_t tx;
        int type_block = block_from_type_cc(cc);

        if (type_block >= 0 &&
            nanocore_spec_editor_type_by_raw(
                (controller_block_t)type_block, value)) {
            /*
             * TYPE CCs are retained only as compact local/NVS mirrors.
             * All editor type/model changes use the device's native SysEx.
             */
            tx = send_editor_type_raw(
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

        esp_err_t tx;
        int type_block = block_from_type_cc(items[i].cc);
        if (type_block >= 0 &&
            nanocore_spec_editor_type_by_raw(
                (controller_block_t)type_block, items[i].value)) {
            tx = send_editor_type_raw(
                (controller_block_t)type_block, items[i].value);
        } else {
            tx = send_cc(items[i].cc, items[i].value);
        }

        if (tx == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(MIDI_ACTION_GAP_MS));
        }
    }
}

static void scene_load_names(void)
{
    s_base_mode_known = controller_storage_base_mode_load(s_runtime.current_preset,
                                                          &s_base_mode) == ESP_OK;
    memset(s_scene_saved, 0, sizeof(s_scene_saved));
    for (uint8_t i = 0; i < CTRL_SCENE_COUNT; ++i) {
        esp_err_t rc = controller_storage_scene_load(
            s_runtime.current_preset, i, &s_scene_name_scratch);
        if (rc == ESP_OK) {
            s_scene_saved[i] = true;
            snprintf(s_preset.scene[i].name, CTRL_NAME_LEN, "%.*s",
                     CTRL_NAME_LEN - 1, s_scene_name_scratch.name);
        }
    }
}

static bool scene_block_visible(bool synth, int block)
{
    return !synth || (block != BLOCK_AMP && block != BLOCK_CAB);
}

static bool scene_values_valid(const float *p, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i)
        if (!(p[i] >= 0.0f && p[i] <= 1.0f)) return false;
    return true;
}

static bool scene_profile_valid(const controller_scene_profile_t *p)
{
    if (p->version != CTRL_PROFILE_VERSION || p->synth_mode > 1 ||
        p->synth_on > 1) return false;
    if (p->synth_mode) {
        const nanocore_effect_type_info_t *v = nanocore_spec_synth_voice(p->synth_voice);
        /* 0x0A controls mode; a separate voice bypass write is unverified. */
        if (!p->synth_on || !v || p->synth_count != v->expected_param_count ||
            p->synth_count > CTRL_PROFILE_SYNTH_PARAMS ||
            !scene_values_valid(p->synth_params, p->synth_count)) return false;
    }
    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!scene_block_visible(p->synth_mode, b)) continue;
        const controller_scene_block_t *r = &p->block[b];
        const nanocore_effect_type_info_t *t =
            nanocore_spec_editor_type_by_raw((controller_block_t)b, r->type);
        if (!t || r->on > 1 || r->count > CTRL_PROFILE_PARAMS ||
            r->count != t->expected_param_count ||
            nanocore_spec_mode_selector_index((controller_block_t)b,
                                              p->synth_mode, r->type) < 0 ||
            !scene_values_valid(r->params, r->count)) return false;
    }
    return true;
}

static void scene_fail(const char *notice)
{
    ESP_LOGW(TAG, "SCENE|FAIL|phase=%u|%s", (unsigned)s_scene_phase, notice);
    /* A rejected save must leave the editor usable so a bad type (e.g.
     * a hidden drive in Synth) can be corrected. Recall faults lock edits
     * until an explicit retry or another scene/mode selection. */
    bool save_failed = s_scene_phase == SCENE_SAVE || s_scene_phase == SCENE_BASE_SAVE;
    s_scene_phase = SCENE_IDLE;
    s_scene_after_models = false;
    s_scene_fault = !save_failed;
    s_sound_origin_valid=false;
    s_discard_action=DISCARD_NONE;
    s_pending_scene_delta = 0;
    s_scene_verified_models = false;
    s_parameter_editing = false;
    show_notice(notice);
}

static bool scene_start_read(scene_phase_t phase)
{
    s_scene_phase = phase;
    catalog_defer(CATALOG_IDLE_MS);
    /* First read sooner; a mismatching read gets the proven longer settle. */
    vTaskDelay(pdMS_TO_TICKS(s_scene_read_retries ? 160 : 50));
    esp_err_t rc = nanocore_protocol_request_live_state_seq(&s_scene_seq);
    if (rc != ESP_OK) {
        scene_fail("SCENE LIVE READ FAILED");
        return false;
    }
    s_scene_deadline = esp_timer_get_time() + 3000000LL;
    return true;
}

static void scene_select_mode(bool synth)
{
    if (scene_busy() ||
        !s_midi_ready || s_recall_pending || s_recall_poll_inflight ||
        s_catalog_inflight || !s_synth_mode_known) {
        show_notice("WAIT LIVE SYNC");
        return;
    }
    if (s_synth_mode == synth && !s_scene_fault) return;
    sound_edit_begin();
    s_scene_target_mode = synth;
    s_scene_verified_models = false;
    s_scene_read_retries = 0;
    s_scene_phase = SCENE_MODE_EDIT;
    s_scene_fault = false;
    if (nanocore_protocol_set_synth_mode(synth) != ESP_OK) {
        scene_fail("SYNTH MODE TX FAILED");
        return;
    }
    queue_notice("CHANGING SOUND MODE");
    (void)scene_start_read(SCENE_MODE_EDIT);
}

static bool scene_near(const float *a, const float *b, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i)
        if (!(fabsf(a[i] - b[i]) <= 0.002f)) return false;
    return true;
}

/* Exact float vectors, not default presets. Type writes also carry params. */
static bool scene_send_profile(void)
{
    const controller_scene_profile_t *p = &s_scene_profile;
    esp_err_t rc;
    if (!s_midi_ready || !s_synth_mode_known ||
        s_synth_mode != (p->synth_mode != 0)) return false;

    if (p->synth_mode &&
        (!s_synth_voice_known || s_synth_voice != p->synth_voice ||
         s_synth_param_count != p->synth_count ||
         !scene_near(s_synth_params, p->synth_params, p->synth_count))) {
        rc = nanocore_protocol_set_synth_voice(p->synth_voice, p->synth_params, p->synth_count);
        if (rc != ESP_OK) return false;
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!scene_block_visible(p->synth_mode, b)) continue;
        controller_block_t block = (controller_block_t)b;
        const controller_scene_block_t *r = &p->block[b];
        bool model = nanocore_spec_editor_is_model_block(block);
        uint8_t raw = 0;
        bool same_type = current_type_raw(block, &raw) && raw == r->type;
        bool same_params = s_block_profile_known[b] &&
            s_block_param_count[b] == r->count &&
            scene_near(s_block_params[b], r->params, r->count);
        bool type_written = false;
        if (model) {
            /* Only elide a model write after a successful same-mode scene
             * recall with an unchanged trusted TX mirror. Cross-mode or
             * unknown state always writes explicitly. */
            if (s_scene_force_models || !same_type) {
                rc = nanocore_protocol_set_block_model(block, r->type);
                if (rc != ESP_OK) return false;
                if (block == BLOCK_AMP) {
                    s_amp_model = r->type;
                    s_amp_model_known = true;
                } else {
                    s_cab_model = r->type;
                    s_cab_model_known = true;
                }
                type_written = true;
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            if (!same_params || type_written) {
                for (uint8_t i = 0; i < r->count; ++i) {
                    if (!type_written && s_block_profile_known[b] &&
                        i < s_block_param_count[b] &&
                        scene_near(&s_block_params[b][i], &r->params[i], 1)) continue;
                    rc = nanocore_protocol_set_block_param(block, i, r->params[i]);
                    if (rc != ESP_OK) return false;
                    vTaskDelay(pdMS_TO_TICKS(MIDI_ACTION_GAP_MS));
                }
            }
        } else if (!same_type || !same_params) {
            unsigned changed = 0;
            if (same_type && s_block_profile_known[b] && s_block_param_count[b] == r->count) {
                for (uint8_t i=0;i<r->count;++i)
                    if (!scene_near(&s_block_params[b][i],&r->params[i],1)) ++changed;
            }
            if (changed == 1 || changed == 2) {
                for (uint8_t i=0;i<r->count;++i) {
                    if (scene_near(&s_block_params[b][i],&r->params[i],1)) continue;
                    rc=nanocore_protocol_set_block_param(block,i,r->params[i]);
                    if (rc!=ESP_OK) return false;
                    vTaskDelay(pdMS_TO_TICKS(MIDI_ACTION_GAP_MS));
                }
            } else {
                rc = nanocore_protocol_set_block_type(block, r->type, r->params, r->count);
                if (rc != ESP_OK) return false;
                type_written = true;
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }
        if (type_written || !s_block_known[b] || s_block_on[b] != (r->on != 0)) {
            rc = send_cc(nanocore_block_onoff_cc(block), r->on ? 127 : 0);
            if (rc != ESP_OK) return false;
            vTaskDelay(pdMS_TO_TICKS(MIDI_ACTION_GAP_MS));
        }
    }
    return true;
}

static void scene_send_and_verify(void)
{
    s_scene_phase = SCENE_VERIFY;
    s_scene_read_retries = 0;
    queue_notice("APPLYING SCENE");
    if (!scene_send_profile()) {
        scene_fail("SCENE EFFECT TX FAILED");
        return;
    }
    (void)scene_start_read(SCENE_VERIFY);
}

static bool scene_verify(const nanocore_protocol_event_t *e)
{
    const controller_scene_profile_t *p = &s_scene_profile;
    if (!e->synth_mode_known || e->synth_mode != (p->synth_mode != 0)) return false;
    if (p->synth_mode &&
        (!e->synth_known || e->synth_on != (p->synth_on != 0) ||
         e->synth_variant != p->synth_voice ||
         e->synth_param_count != p->synth_count ||
         !scene_near(e->synth_params, p->synth_params, p->synth_count))) {
        ESP_LOGW(TAG, "SCENE|VERIFY_MISMATCH|SYNTH");
        return false;
    }
    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!scene_block_visible(p->synth_mode, b)) continue;
        const controller_scene_block_t *r = &p->block[b];
        bool model = b == BLOCK_AMP || b == BLOCK_CAB;
        if (!e->block_known[b] || !e->block_profile_known[b] ||
            e->block_on[b] != (r->on != 0) ||
            (!model && e->block_variant[b] != r->type) ||
            e->block_param_count[b] != r->count ||
            !scene_near(e->block_params[b], r->params, r->count)) {
            ESP_LOGW(TAG, "SCENE|VERIFY_MISMATCH|block=%d", b);
            return false;
        }
        /* 0x63 cannot report live AMP/CAB model indices: TX/local mirror
         * is all we have. Never claim an independent model readback. */
    }
    return true;
}

static void scene_request_save(void)
{
    if (!s_midi_ready || !s_synth_mode_known || scene_busy() ||
        s_scene_fault || s_recall_pending || s_catalog_inflight) {
        show_notice("WAIT SYNC / RETRY SCENE");
        return;
    }
    s_scene_read_retries = 0;
    s_scene_phase = SCENE_SAVE;
    show_notice("CAPTURING SCENE");
    (void)scene_start_read(SCENE_SAVE);
}

static void scene_save_from_live(const nanocore_protocol_event_t *e)
{
    controller_scene_profile_t *p = &s_scene_profile;
    memset(p, 0, sizeof(*p));
    if (!e->synth_mode_known || !s_name_editor.active || !s_name_target_scene) {
        scene_fail("SCENE MODE UNKNOWN");
        return;
    }
    p->version = CTRL_PROFILE_VERSION;
    p->synth_mode = e->synth_mode;
    p->synth_on = e->synth_on;
    p->synth_voice = e->synth_variant;
    p->synth_count = e->synth_param_count;
    memcpy(p->synth_params, e->synth_params, sizeof(p->synth_params));
    snprintf(p->name, sizeof(p->name), "%.*s",
             CTRL_NAME_LEN - 1, s_name_editor.text);
    for (int b = 0; b < BLOCK_COUNT; ++b) {
        if (!scene_block_visible(p->synth_mode, b)) continue;
        if (!e->block_known[b] || !e->block_profile_known[b] ||
            (b == BLOCK_AMP && !s_amp_model_known) ||
            (b == BLOCK_CAB && !s_cab_model_known)) {
            scene_fail("SCENE PROFILE INCOMPLETE");
            return;
        }
        controller_scene_block_t *r = &p->block[b];
        r->on = e->block_on[b];
        r->type = b == BLOCK_AMP ? s_amp_model :
                  b == BLOCK_CAB ? s_cab_model : e->block_variant[b];
        r->count = e->block_param_count[b];
        memcpy(r->params, e->block_params[b], sizeof(r->params));
    }
    if (!scene_profile_valid(p)) {
        scene_fail("SCENE TYPE/VOICE NOT VALID");
        return;
    }
    esp_err_t rc = controller_storage_scene_save(s_runtime.current_preset,
                                                  s_runtime.current_scene, p);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "SCENE|STORAGE_ERROR|%s", esp_err_to_name(rc));
        scene_fail("SCENE SAVE FAILED");
        return;
    }
    s_scene_saved[s_runtime.current_scene] = true;
    snprintf(s_preset.scene[s_runtime.current_scene].name, CTRL_NAME_LEN,
             "%.*s", CTRL_NAME_LEN - 1, p->name);
    s_preset.active_scene = s_runtime.current_scene;
    clear_edit_dirty();
    s_scene_phase = SCENE_IDLE;
    s_scene_fault = false;
    name_editor_close(&s_name_editor);
    s_screen = UI_SCREEN_PERFORMANCE;
    (void)persist_runtime();
    ESP_LOGI(TAG, "SCENE|SAVED|preset=%u|scene=%c|mode=%u|name=%s",
             s_runtime.current_preset + 1, 'A' + s_runtime.current_scene,
             p->synth_mode, p->name);
    show_notice("SCENE SAVED TO CONTROLLER");
}

static void scene_handle_live(const nanocore_protocol_event_t *e)
{
    scene_phase_t phase = s_scene_phase;
    if (phase == SCENE_MODE_EDIT || phase == SCENE_MODE_RECALL ||
        phase == SCENE_BASE_RESTORE) {
        if (!e->synth_mode_known || e->synth_mode != s_scene_target_mode) {
            if (s_scene_read_retries++ < 2) {
                ESP_LOGW(TAG, "SCENE|MODE_WAIT|retry=%u", s_scene_read_retries);
                (void)scene_start_read(phase);
                return;
            }
            scene_fail("SYNTH MODE NOT CONFIRMED");
            return;
        }
        ESP_LOGI(TAG, "SCENE|MODE_OK|mode=%u", e->synth_mode);
        if (phase == SCENE_BASE_RESTORE) {
            s_scene_phase = SCENE_IDLE;
            s_scene_fault = false;
            clear_edit_dirty();
            ESP_LOGI(TAG,"BASE|MODE_RESTORED|mode=%u",e->synth_mode);
            show_notice(e->synth_mode ? "PRESET SYNTH MODE" : "PRESET GUITAR MODE");
        } else if (phase == SCENE_MODE_EDIT) {
            s_scene_phase = SCENE_IDLE;
            s_scene_fault = false;
            sound_edit_confirm(e);
            show_notice(e->synth_mode ? "SYNTH MODE" : "GUITAR MODE");
        } else {
            scene_send_and_verify();
        }
    } else if (phase == SCENE_VERIFY) {
        if (!scene_verify(e)) {
            if (s_scene_read_retries++ < 1) {
                (void)scene_start_read(SCENE_VERIFY);
                return;
            }
            scene_fail("SCENE VERIFY FAILED");
            return;
        }
        s_scene_phase = SCENE_IDLE;
        s_scene_fault = false;
        clear_edit_dirty();
        s_scene_verified_models = !s_scene_profile.synth_mode &&
                                  s_amp_model_known && s_cab_model_known;
        s_preset.active_scene = s_runtime.current_scene;
        (void)persist_runtime();
        ESP_LOGI(TAG,"SCENE|LATENCY|ms=%lu",(unsigned long)((esp_timer_get_time()-s_scene_started_us)/1000));
        ESP_LOGI(TAG, "SCENE|READY|preset=%u|scene=%c|mode=%u|name=%s",
                 s_runtime.current_preset + 1, 'A' + s_runtime.current_scene,
                 s_scene_profile.synth_mode, s_scene_profile.name);
        show_notice("SCENE READY");
    } else if (phase == SCENE_SAVE) {
        scene_save_from_live(e);
    } else if (phase == SCENE_BASE_SAVE) {
        if (!e->synth_mode_known || e->synth_mode != s_scene_target_mode) {
            s_screen = UI_SCREEN_NAME_EDITOR;
            scene_fail("PRESET MODE CHANGED - CHECK");
            return;
        }
        s_scene_phase = SCENE_IDLE;
        s_scene_fault = false;
        commit_base_save();
    }
}

static void apply_current_scene(void)
{
    if (s_runtime.current_scene >= CTRL_SCENE_COUNT)
        s_runtime.current_scene = 0;
    if (!s_midi_ready || s_recall_pending || s_recall_poll_inflight ||
        s_scene_phase != SCENE_IDLE || s_catalog_inflight) {
        show_notice("WAIT LIVE SYNC");
        return;
    }

    esp_err_t rc = controller_storage_scene_load(s_runtime.current_preset,
                           s_runtime.current_scene, &s_scene_profile);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        /* Legacy/unused scenes have no independent full snapshot.
         * Preserve the existing guitar sparse overlay behavior. New
         * scenes begin with the current sound until explicitly saved. */
        if (!s_synth_mode) {
            size_t count = 0;
            const midi_assignment_t *items = scene_assignments(s_runtime.current_scene, &count);
            apply_assignments(items, count);
        }
        s_scene_fault = false;
        s_preset.active_scene = s_runtime.current_scene;
        (void)persist_runtime();
        ESP_LOGI(TAG, "SCENE|UNSAVED|preset=%u|scene=%c",
                 s_runtime.current_preset + 1, 'A' + s_runtime.current_scene);
        show_notice("SCENE NOT CAPTURED");
        return;
    }
    if (rc != ESP_OK || !scene_profile_valid(&s_scene_profile)) {
        scene_fail("SCENE STORAGE/PROFILE ERROR");
        return;
    }
    if (!s_synth_mode_known) {
        scene_fail("SCENE MODE UNKNOWN");
        return;
    }
    s_scene_fault = false;
    s_scene_target_mode = s_scene_profile.synth_mode != 0;
    s_scene_force_models = !s_scene_verified_models || s_synth_mode != s_scene_target_mode;
    s_scene_verified_models = false;
    s_scene_started_us = esp_timer_get_time();
    s_scene_read_retries = 0;
    ESP_LOGI(TAG, "SCENE|APPLY|preset=%u|scene=%c|mode=%u",
             s_runtime.current_preset + 1, 'A' + s_runtime.current_scene,
             s_scene_profile.synth_mode);
    if (s_synth_mode != s_scene_target_mode) {
        s_scene_phase = SCENE_MODE_RECALL;
        if (nanocore_protocol_set_synth_mode(s_scene_target_mode) != ESP_OK) {
            scene_fail("SCENE MODE TX FAILED");
            return;
        }
        queue_notice("CHANGING SOUND MODE");
        (void)scene_start_read(SCENE_MODE_RECALL);
    } else {
        scene_send_and_verify();
    }
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
    scene_load_names();
    s_scene_fault = false;
    s_pending_scene_delta=0;
    s_runtime.current_scene =
        s_preset.active_scene < CTRL_SCENE_COUNT
            ? s_preset.active_scene : 0;

    tempo_engine_set_bpm(s_preset.bpm);
    status_leds_set_bpm(s_preset.bpm);

    clear_edit_dirty();
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

static bool discard_reload_local(void)
{
    controller_preset_t saved;
    esp_err_t rc=controller_storage_load_preset(s_runtime.current_preset,&saved);
    if(rc!=ESP_OK) {
        show_notice("DISCARD FAILED - NVS READ");
        return false;
    }
    bool base_known=s_base_mode_known;
    bool base_mode=s_base_mode;
    s_preset=saved;
    scene_load_names();
    if(!s_base_mode_known && base_known) {
        s_base_mode_known=true;
        s_base_mode=base_mode;
    }
    s_preset.active_scene=s_runtime.current_scene;
    tempo_engine_set_bpm(s_preset.bpm);
    status_leds_set_bpm(s_preset.bpm);
    s_pending_scene_delta=0;
    s_scene_fault=false;
    s_screen=UI_SCREEN_PERFORMANCE;
    s_parameter_editing=false;
    s_selected_parameter=0;
    clear_edit_dirty();
    return true;
}

static bool navigation_ready(void)
{
    if(!s_midi_ready || scene_busy() || s_recall_pending ||
       s_recall_poll_inflight || s_catalog_inflight) {
        show_notice("WAIT LIVE SYNC");
        return false;
    }
    return true;
}

static void change_preset(int delta)
{
    if(!navigation_ready()) return;
    if(!discard_confirm(delta<0?DISCARD_PRESET_DOWN:DISCARD_PRESET_UP)) return;

    int p = (int)s_runtime.current_preset + delta;
    if (p < NANOCORE_PRESET_MIN) p = NANOCORE_PRESET_MAX;
    if (p > NANOCORE_PRESET_MAX) p = NANOCORE_PRESET_MIN;

    ESP_LOGI(TAG, "Preset -> %d", p);
    (void)load_preset((uint8_t)p, true);
}

static void change_scene(int delta)
{
    if (!scene_busy() && s_midi_ready && !s_recall_pending &&
        s_catalog_inflight && !s_dirty) {
        s_pending_scene_delta=(s_pending_scene_delta+delta)%CTRL_SCENE_COUNT;
        return;
    }
    if (scene_busy() || !s_midi_ready || s_recall_pending || s_catalog_inflight) {
        show_notice("WAIT SCENE SYNC");
        return;
    }
    if(s_recall_poll_inflight) {
        show_notice("WAIT SCENE SYNC");
        return;
    }
    bool discard=s_dirty;
    if(!discard_confirm(delta<0?DISCARD_SCENE_DOWN:DISCARD_SCENE_UP)) return;
    if(discard && !discard_reload_local()) return;
    s_screen = UI_SCREEN_PERFORMANCE;
    s_parameter_editing = false;
    int s = (int)s_runtime.current_scene + delta;
    while (s < 0) s += CTRL_SCENE_COUNT;
    while (s >= CTRL_SCENE_COUNT) s -= CTRL_SCENE_COUNT;

    s_runtime.current_scene = (uint8_t)s;
    if(discard) {
        /* A new/unrecorded destination must not inherit discarded edits. */
        s_preset.active_scene=s_runtime.current_scene;
        (void)persist_runtime();
        restore_current_preset_to_nanocore();
    } else {
        apply_current_scene();
    }
}

static void cycle_mode(void)
{
    if(!navigation_ready()) return;
    bool discard=s_dirty;
    if(!discard_confirm(DISCARD_MODE)) return;
    if(discard && !discard_reload_local()) return;
    s_pending_scene_delta=0;
    s_parameter_editing = false;
    s_screen = UI_SCREEN_PERFORMANCE;

    controller_mode_t previous = s_runtime.mode;
    s_runtime.mode =
        (controller_mode_t)((s_runtime.mode + 1) % CTRL_MODE_COUNT);

    status_leds_set_mode(s_runtime.mode);
    (void)persist_runtime();

    ESP_LOGI(TAG, "MODE -> %u", (unsigned)s_runtime.mode);

    if (discard) {
        /* In all controller modes discard means physical base recall.
         * The existing recall state machine overlays SCENE only afterward. */
        restore_current_preset_to_nanocore();
    } else if (s_runtime.mode == CTRL_MODE_SCENE) {
        /* Remember base mode before overlaying scenes, even for old presets
         * without a stored mode preference. */
        if (s_synth_mode_known && !s_base_mode_known) {
            s_base_mode = s_synth_mode;
            s_base_mode_known = true;
        }
        apply_current_scene();
    } else if (previous == CTRL_MODE_SCENE) {
        s_scene_fault = false;
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
        mark_edit_dirty();
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
    s_scene_verified_models = false;
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
    if (delta == 0 || scene_busy() || s_scene_fault || s_selected_block >= BLOCK_COUNT) return;
    if ((s_runtime.mode == CTRL_MODE_SCENE || s_synth_mode) &&
        (!s_midi_ready || !s_synth_mode_known || s_recall_pending)) return;
    s_scene_verified_models = false;

    if (s_synth_selected) {
        if (!s_synth_voice_known) return;
        if (s_selected_parameter == 0) {
            show_notice("SET MODE ON PERFORMANCE ENC2");
            return;
        }
        const nanocore_effect_type_info_t *voice = nanocore_spec_synth_voice(s_synth_voice);
        if (!voice) return;
        esp_err_t rc;
        if (s_selected_parameter == 1) {
            int next = ((int)s_synth_voice + delta) % (int)nanocore_spec_synth_voice_count();
            if (next < 0) next += (int)nanocore_spec_synth_voice_count();
            const nanocore_effect_type_info_t *v = nanocore_spec_synth_voice((uint8_t)next);
            float defaults[NC_PROTO_SYNTH_MAX_PARAMS] = {0};
            if (!v || !nanocore_spec_editor_defaults(v, defaults, NC_PROTO_SYNTH_MAX_PARAMS)) return;
            rc = nanocore_protocol_set_synth_voice(v->id, defaults, v->expected_param_count);
            if (rc == ESP_OK) {
                s_synth_voice = v->id;
                s_synth_param_count = v->expected_param_count;
                memcpy(s_synth_params, defaults, sizeof(s_synth_params));
            }
        } else {
            size_t i = s_selected_parameter - 2;
            if (i >= voice->expected_param_count || i >= s_synth_param_count) return;
            float next = nanocore_spec_adjust_normalized(&voice->params[i], s_synth_params[i], delta);
            rc = nanocore_protocol_set_synth_param((uint8_t)i, next);
            if (rc == ESP_OK) s_synth_params[i] = next;
        }
        if (rc == ESP_OK) {
            mark_edit_dirty();
            render_ui();
        } else show_notice("SYNTH WRITE FAILED");
        return;
    }

    /* 0=ON/OFF, 1=TYPE/MODEL, 2..=live type-local parameters. */
    if (s_selected_parameter == 0) {
        uint8_t current =
            (s_block_known[s_selected_block] &&
             s_block_on[s_selected_block]) ? 127 : 0;
        uint8_t next = delta > 0 ? 127 : 0;

        if (next != current) {
            (void)set_working_cc(
                nanocore_block_onoff_cc(s_selected_block), next);
        }
        return;
    }

    if (s_selected_parameter == 1) {
        int current = current_type_selector(s_selected_block);
        if (current < 0) current = delta > 0 ? -1 : 0;

        const int count =
            (int)nanocore_spec_mode_type_count(s_selected_block, s_synth_mode);
        if (count <= 0) return;

        int next = current + delta;
        while (next < 0) next += count;
        while (next >= count) next -= count;

        const nanocore_effect_type_info_t *info =
            nanocore_spec_mode_type_by_selector(
                s_selected_block, s_synth_mode, (size_t)next);
        if (!info) return;

        (void)set_working_cc(
            nanocore_block_type_cc(s_selected_block),
            info->id);
        return;
    }

    const nanocore_effect_type_info_t *info =
        current_type_info(s_selected_block);
    if (!info) return;

    if (s_selected_block == BLOCK_DELAY) {
        const size_t bpm_index = 2 + info->expected_param_count;
        const size_t note_index = bpm_index + 1;

        if (s_selected_parameter == bpm_index) {
            int bpm = (int)s_preset.bpm + delta;
            if (bpm < 30) bpm = 30;
            if (bpm > 300) bpm = 300;

            s_preset.bpm = (uint16_t)bpm;
            tempo_engine_set_bpm(s_preset.bpm);
            status_leds_set_bpm(s_preset.bpm);
            mark_edit_dirty();
            update_delay_from_tempo();
            return;
        }

        if (s_selected_parameter == note_index) {
            int sub = (int)s_preset.delay_subdivision + delta;
            while (sub < 0) sub += DELAY_SUBDIV_COUNT;
            while (sub >= DELAY_SUBDIV_COUNT) sub -= DELAY_SUBDIV_COUNT;

            s_preset.delay_subdivision = (delay_subdivision_t)sub;
            mark_edit_dirty();
            update_delay_from_tempo();
            return;
        }
    }
    if (!info->params) return;

    uint8_t param_index =
        (uint8_t)(s_selected_parameter - 2);
    if (param_index >= info->expected_param_count ||
        param_index >= NC_PROTO_MAX_PARAMS) {
        return;
    }

    float current = 0.0f;
    bool have_current =
        s_block_profile_known[s_selected_block] &&
        param_index < s_block_param_count[s_selected_block];

    if (have_current) {
        current = s_block_params[s_selected_block][param_index];
    } else {
        float defaults[NC_PROTO_MAX_PARAMS] = {0};
        if (!nanocore_spec_editor_defaults(
                info, defaults, NC_PROTO_MAX_PARAMS)) {
            return;
        }
        current = defaults[param_index];
    }

    float next = nanocore_spec_adjust_normalized(
        &info->params[param_index], current, delta);

    if (set_profile_param(
            s_selected_block, param_index, next) != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s parameter write failed type=%u param=%u",
                 nanocore_block_name(s_selected_block),
                 info->id,
                 param_index);
    }

    render_ui();
}

static void open_block_editor(void)
{
    s_selected_parameter = 0;
    s_parameter_editing = false;
    s_screen = UI_SCREEN_BLOCK_EDITOR;

    /*
     * Every Stage 07 editor starts from an authoritative 0x63 live snapshot.
     * AMP/CAB additionally need the saved-preset 0x41 footer for model id.
     */
    if (s_midi_ready &&
        !s_recall_pending && !s_recall_poll_inflight) {
        s_editor_models_after_live =
            (s_selected_block == BLOCK_AMP ||
             s_selected_block == BLOCK_CAB) &&
            !((s_selected_block == BLOCK_AMP && s_amp_model_known) ||
              (s_selected_block == BLOCK_CAB && s_cab_model_known));

        esp_err_t rc = nanocore_protocol_request_live_state();
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "%s editor live refresh failed: %s",
                     nanocore_block_name(s_selected_block),
                     esp_err_to_name(rc));
            s_editor_models_after_live = false;
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
        scene_request_save();
        return;
    }
    if (!s_midi_ready || !s_synth_mode_known || scene_busy() ||
        s_recall_pending || s_catalog_inflight || s_scene_fault) {
        show_notice("WAIT LIVE SYNC / NOT SAVED");
        return;
    }
    /* Confirm the active physical slot and sound mode before a real SAVE. */
    s_scene_target_mode=s_synth_mode;
    s_scene_read_retries=0;
    s_scene_phase=SCENE_BASE_SAVE;
    queue_notice("CHECKING PRESET");
    (void)scene_start_read(SCENE_BASE_SAVE);
}

static void commit_base_save(void)
{
    if (!s_name_editor.active || s_name_target_scene) return;
    /*
     * NanoCore's real preset-name write limit is 8 characters.
     * Keep the controller's mirror identical to what the pedal stores.
     */
    char device_name[9] = {0};
    strncpy(device_name, s_name_editor.text, 8);

    if (!s_midi_ready || !s_synth_mode_known || scene_busy() ||
        s_recall_pending || s_catalog_inflight || s_scene_fault) {
        show_notice("WAIT LIVE SYNC / NOT SAVED");
        return;
    }
    if (s_synth_mode && (!s_synth_voice_known || !s_synth_voice_on ||
        !current_type_info(BLOCK_FX2))) {
        show_notice("CHECK SYNTH VOICE / FX2");
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

    s_base_mode = s_synth_mode;
    s_base_mode_known = true;
    rc = controller_storage_base_mode_save(s_runtime.current_preset, s_base_mode);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG,"BASE|MODE_STORE_FAILED|%s",esp_err_to_name(rc));
        show_notice("PEDAL SAVED / MODE NVS ERROR");
        return;
    }
    ESP_LOGI(TAG,"BASE|SAVED|preset=%u|mode=%u",s_runtime.current_preset+1,s_base_mode);
    clear_edit_dirty();
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
        .synth_mode_known = s_synth_mode_known,
        .synth_mode = s_synth_mode,
        .synth_selected = s_synth_selected,
        .levels_selected=s_levels_selected,
        .levels_known=s_levels_known,
        .levels_wait=s_levels_wait,
        .levels_edit=s_levels_edit,
        .levels_row=s_levels_row,
        .input_gain_db=s_input_gain_db,
        .level_raw_known=s_level_raw_known,
        .level_raw=s_level_raw,
        .synth_voice_known = s_synth_voice_known,
        .synth_voice_on = s_synth_voice_on,
        .editor_read_only = scene_busy() || s_scene_fault ||
                            (s_synth_mode && !s_synth_mode_known),
        .scene_busy = scene_busy(),
        .scene_fault = s_scene_fault,
        .scene_captured = s_scene_saved[s_runtime.current_scene],
        .selected_parameter = s_selected_parameter,
        .parameter_editing = s_parameter_editing,
        .notice = s_discard_action != DISCARD_NONE ? discard_message() : s_notice,
        .name_editor = &s_name_editor,
    };

    memcpy(u.block_known, s_block_known, sizeof(u.block_known));
    memcpy(u.block_on, s_block_on, sizeof(u.block_on));

    char parameter_text[72] = {0};

    if(s_screen==UI_SCREEN_LEVELS) { display_ui_render(&u); return; }
    if (s_synth_selected) {
        const nanocore_effect_type_info_t *voice =
            s_synth_voice_known ? nanocore_spec_synth_voice(s_synth_voice) : NULL;
        u.editor_override = true;
        u.editor_parameter_count = editor_parameter_count();
        u.editor_parameter_labels[0] = "ON/OFF";
        u.editor_parameter_labels[1] = "TYPE";
        if (voice) {
            for (uint8_t i = 0; i < voice->expected_param_count; ++i)
                u.editor_parameter_labels[i + 2] = voice->params[i].name;
        }
        if (s_selected_parameter >= u.editor_parameter_count)
            s_selected_parameter = u.editor_parameter_count - 1;
        u.selected_parameter = s_selected_parameter;
        u.selected_parameter_label = u.editor_parameter_labels[s_selected_parameter];
        if (s_synth_voice_known && s_selected_parameter == 0) {
            u.parameter_known = true;
            u.parameter_text = s_synth_voice_on ? "ON" : "OFF";
        } else if (voice && s_selected_parameter == 1) {
            u.parameter_known = true;
            u.parameter_text = voice->name;
        } else if (voice && s_selected_parameter >= 2 &&
                   s_selected_parameter - 2 < s_synth_param_count &&
                   s_selected_parameter - 2 < voice->expected_param_count) {
            size_t i = s_selected_parameter - 2;
            u.parameter_known = true;
            nanocore_spec_format_normalized(&voice->params[i], s_synth_params[i],
                                           parameter_text, sizeof(parameter_text));
            u.parameter_text = parameter_text;
        }
        display_ui_render(&u);
        return;
    }

    const nanocore_effect_type_info_t *info =
        current_type_info(s_selected_block);

    u.editor_override = true;
    u.editor_parameter_count = editor_parameter_count();
    u.editor_parameter_labels[0] = "ON/OFF";
    u.editor_parameter_labels[1] =
        nanocore_spec_editor_is_model_block(s_selected_block)
            ? "MODEL" : "TYPE";

    if (info && info->params) {
        for (uint8_t i = 0;
             i < info->expected_param_count &&
             (size_t)(i + 2) < UI_MAX_EDITOR_PARAMETERS;
             ++i) {
            u.editor_parameter_labels[i + 2] =
                info->params[i].name;
        }

        if (s_selected_block == BLOCK_DELAY) {
            size_t bpm_index = 2 + info->expected_param_count;
            size_t note_index = bpm_index + 1;
            if (bpm_index < UI_MAX_EDITOR_PARAMETERS)
                u.editor_parameter_labels[bpm_index] = "TEMPO BPM";
            if (note_index < UI_MAX_EDITOR_PARAMETERS)
                u.editor_parameter_labels[note_index] = "NOTE";
        }
    }

    if (u.editor_parameter_count == 0) {
        u.editor_parameter_count = 2;
    }

    if (s_selected_parameter >= u.editor_parameter_count) {
        s_selected_parameter = u.editor_parameter_count - 1;
        u.selected_parameter = s_selected_parameter;
    }

    if (s_selected_parameter < u.editor_parameter_count) {
        u.selected_parameter_label =
            u.editor_parameter_labels[s_selected_parameter];
    }

    if (s_selected_parameter == 0) {
        u.parameter_known = s_block_known[s_selected_block];
        u.parameter_value =
            s_block_on[s_selected_block] ? 127 : 0;

        if (u.parameter_known) {
            snprintf(parameter_text, sizeof(parameter_text), "%s",
                     s_block_on[s_selected_block] ? "ON" : "OFF");
            u.parameter_text = parameter_text;
        }
    } else if (s_selected_parameter == 1) {
        int selector = current_type_selector(s_selected_block);
        u.parameter_known = selector >= 0;

        if (selector >= 0) {
            u.parameter_value = (uint16_t)selector;
            const nanocore_effect_type_info_t *selected_type =
                nanocore_spec_mode_type_by_selector(
                    s_selected_block, s_synth_mode, (size_t)selector);
            if (selected_type) {
                if (nanocore_spec_editor_is_model_block(s_selected_block)) {
                    format_model_name(s_selected_block, selected_type->id,
                                      parameter_text, sizeof(parameter_text));
                    u.parameter_text = parameter_text;
                } else {
                    u.parameter_text = selected_type->name;
                }
            }
        } else if (s_synth_mode && s_block_profile_known[s_selected_block]) {
            u.parameter_known = true;
            snprintf(parameter_text, sizeof(parameter_text), "RAW %u UNAVAILABLE",
                     s_block_variant[s_selected_block]);
            u.parameter_text = parameter_text;
        }
    } else if (info && s_selected_block == BLOCK_DELAY &&
               s_selected_parameter ==
                   (size_t)(2 + info->expected_param_count)) {
        u.parameter_known = true;
        snprintf(parameter_text, sizeof(parameter_text),
                 "%u BPM", s_preset.bpm);
        u.parameter_text = parameter_text;
    } else if (info && s_selected_block == BLOCK_DELAY &&
               s_selected_parameter ==
                   (size_t)(3 + info->expected_param_count)) {
        u.parameter_known = true;
        snprintf(parameter_text, sizeof(parameter_text), "%s",
                 tempo_engine_subdivision_name(
                     s_preset.delay_subdivision));
        u.parameter_text = parameter_text;
    } else if (info && info->params) {
        uint8_t param_index =
            (uint8_t)(s_selected_parameter - 2);

        if (s_block_profile_known[s_selected_block] &&
            param_index < s_block_param_count[s_selected_block] &&
            param_index < info->expected_param_count) {

            u.parameter_known = true;
            nanocore_spec_format_normalized(
                &info->params[param_index],
                s_block_params[s_selected_block][param_index],
                parameter_text,
                sizeof(parameter_text));
            u.parameter_text = parameter_text;
        } else {
            u.parameter_known = false;
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
            catalog_link_changed(true);
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
        catalog_link_changed(false);
        s_tuner_on = false;
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
    if (delta == 0 || s_synmap_enabled || scene_busy()) return;
    if(s_discard_action!=DISCARD_NONE) {
        discard_cancel();
        render_ui();
        return; /* First rotation cancels only, never edits. */
    }
    if(s_screen==UI_SCREEN_LEVELS) {
        if(encoder==1 && !s_levels_edit) s_levels_row=(uint8_t)(delta>0?1:0);
        else if(encoder==2 && s_levels_edit && s_levels_row==0 && s_levels_known && s_levels_write_ready &&
                !s_levels_wait && !s_slot_probe_inflight && s_midi_ready) {
            int next=(int)s_input_gain_db+(delta>0?1:-1);
            if(s_input_gain_db<0 || s_input_gain_db>20 || next<0) {
                show_notice("NEGATIVE GAIN NOT VERIFIED");
                return;
            }
            if(next<=20 && nanocore_protocol_set_input_gain((int8_t)next,&s_levels_seq)==ESP_OK) {
                s_levels_wait=true;
                s_levels_write_pending=true;
                s_levels_target_db=(int8_t)next;
                s_levels_deadline=esp_timer_get_time()+2500000LL;
            }
        }
        render_ui();
        return;
    }
    if(s_slot_probe_pending || s_slot_probe_inflight || s_levels_wait) return;
    if (encoder == 2 && s_screen == UI_SCREEN_PERFORMANCE &&
        !s_tuner_on &&
        !tempo_engine_tap_active()) {
        scene_select_mode(delta > 0);
        return;
    }

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
            int count=s_synth_mode?7:BLOCK_COUNT;
            int index=s_levels_selected?count:(int)s_selected_block;
            if(s_synth_mode && !s_levels_selected) {
                index=0;
                if(!s_synth_selected) {
                    for(int i=1;i<7;++i)
                        if(s_synth_visible[i]==s_selected_block){index=i;break;}
                }
            }
            index=(index+delta)%(count+1);
            if(index<0) index+=count+1;
            s_levels_selected=index==count;
            if(!s_levels_selected) {
                s_synth_selected=s_synth_mode && index==0;
                s_selected_block=s_synth_mode?s_synth_visible[index]:(controller_block_t)index;
            }
            render_ui();
        }
    } else if (encoder == 2) {
        if (s_screen == UI_SCREEN_BLOCK_EDITOR && s_parameter_editing) {
            adjust_parameter(delta);
        }
    }
}

/* Stage 08A discovery uses the existing TOP+TOP / ENC2 gesture, but
 * captures the full live payload rather than targeting one legacy block.
 * No controller writes are allowed while this temporary mode is active. */
static void handle_input(input_event_t e)
{
    if (scene_busy()) {
        /* Avoid a full TFT redraw during a pending read; allow a deliberate
         * next/previous scene command to be consumed once recall finishes. */
        if (s_runtime.mode==CTRL_MODE_SCENE && !s_dirty &&
            (s_scene_phase==SCENE_MODE_RECALL || s_scene_phase==SCENE_VERIFY)) {
            if(e==INPUT_FS_TOP_LEFT) s_pending_scene_delta=(s_pending_scene_delta-1)%CTRL_SCENE_COUNT;
            if(e==INPUT_FS_TOP_RIGHT) s_pending_scene_delta=(s_pending_scene_delta+1)%CTRL_SCENE_COUNT;
        }
        return;
    }
    if(s_discard_action!=DISCARD_NONE) {
        bool same=(s_discard_action==DISCARD_MODE && e==INPUT_FS_MODE_SHORT) ||
            (s_runtime.mode!=CTRL_MODE_STOMP &&
             ((s_discard_action==DISCARD_PRESET_DOWN && e==INPUT_FS_PRESET_DOWN) ||
              (s_discard_action==DISCARD_PRESET_UP && e==INPUT_FS_PRESET_UP))) ||
            (s_runtime.mode==CTRL_MODE_SCENE &&
             ((s_discard_action==DISCARD_SCENE_DOWN && e==INPUT_FS_TOP_LEFT) ||
              (s_discard_action==DISCARD_SCENE_UP && e==INPUT_FS_TOP_RIGHT)));
        if(!same) {
            discard_cancel();
            render_ui();
            /* Save chord keeps its normal function; every other input
             * cancels only, avoiding a stomp or a different navigation. */
            if(e!=INPUT_CHORD_SAVE) return;
        }
    }
    if (s_synmap_enabled) {
        if (e == INPUT_CHORD_MAPPER) {
            if (s_synmap_inflight) {
                show_notice("CAPTURE IN PROGRESS");
                return;
            }
            s_synmap_enabled = false;
            ESP_LOGI(TAG, "SYNMAP|OFF");
            show_notice("SYNTH MAP OFF");
            if (s_midi_ready) {
                (void)nanocore_protocol_request_live_state();
            }
        } else if (e == INPUT_ENC2_CLICK && !s_synmap_inflight) {
            esp_err_t rc = nanocore_protocol_request_synth_capture(&s_synmap_seq);
            if (rc == ESP_OK) {
                s_synmap_inflight = true;
                s_synmap_sent_us = esp_timer_get_time();
                show_notice("SYNTH CAPTURE");
            } else {
                ESP_LOGW(TAG, "SYNMAP|TX_FAILED|%s", esp_err_to_name(rc));
                show_notice("CAPTURE FAILED");
            }
        }
        return;
    }

    if (e == INPUT_CHORD_MAPPER && s_screen == UI_SCREEN_PERFORMANCE &&
        !s_tuner_on && !nanocore_mapper_is_enabled()) {
        if (!s_midi_ready || s_recall_pending || s_recall_poll_inflight ||
            s_catalog_inflight || s_catalog_cursor < CATALOG_TOTAL ||
            s_runtime.mode == CTRL_MODE_SCENE || tempo_engine_tap_active()) {
            show_notice("WAIT SYNC - PRESET MODE");
            return;
        }
        s_synmap_enabled = true;
        s_synmap_inflight = false;
        ESP_LOGI(TAG, "SYNMAP|ON|ENC2 manual capture; controller writes locked");
        show_notice("SYNTH MAP - ENC2 CAPTURE");
        return;
    }

    if(s_screen==UI_SCREEN_LEVELS) {
        if(e==INPUT_ENC2_CLICK) {
            s_levels_edit=false;
            s_screen=UI_SCREEN_PERFORMANCE;
            nanocore_protocol_set_level_diagnostics(false);
            s_notice[0]='\0';
        } else if(e==INPUT_ENC1_CLICK) {
            if(s_levels_row==1) { s_levels_edit=false; slot_probe_request(true); }
            else if(!s_levels_known || !s_levels_write_ready) levels_read();
            else if(!s_levels_wait) s_levels_edit=!s_levels_edit;
        }
        render_ui();
        return;
    }
    if(s_slot_probe_pending || s_slot_probe_inflight || s_levels_wait) {
        if(e==INPUT_ENC2_CLICK) slot_probe_request(true);
        return;
    }
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

    /* All controller modes may edit either sound engine. */
    if (s_synth_mode && !s_synth_mode_known && e != INPUT_ENC2_CLICK &&
        e != INPUT_FS_MODE_SHORT && e != INPUT_FS_PRESET_UP &&
        e != INPUT_FS_PRESET_DOWN) {
        show_notice("WAIT LIVE SYNC");
        return;
    }

    switch (e) {
    case INPUT_ENC1_CLICK:
        if(s_screen==UI_SCREEN_PERFORMANCE && s_levels_selected) {
            s_screen=UI_SCREEN_LEVELS;
            s_levels_row=0;
            s_levels_edit=false;
            nanocore_protocol_set_level_diagnostics(true);
            levels_read();
            render_ui();
            break;
        }
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            if (s_scene_fault || (s_synth_mode && !s_synth_mode_known)) {
                show_notice("READ ONLY / RETRY SCENE");
                return;
            }
            s_parameter_editing = !s_parameter_editing;
            render_ui();
        } else {
            open_block_editor();
        }
        break;

    case INPUT_ENC2_CLICK:
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            back_from_block_editor();
        } else if (s_screen == UI_SCREEN_PERFORMANCE && s_midi_ready &&
                   !s_recall_pending && !s_recall_poll_inflight &&
                   !s_catalog_inflight) {
            /* Retry a failed recall by reapplying the selected snapshot. */
            if (s_runtime.mode == CTRL_MODE_SCENE && s_scene_fault && s_synth_mode_known &&
                s_scene_saved[s_runtime.current_scene]) {
                apply_current_scene();
                break;
            }
            /* Fresh matched read can recover a stale UI without PC. */
            slot_probe_request(true);
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

    if (e->type == NC_PROTO_EVENT_SYNTH_CAPTURE) {
        if (s_synmap_enabled && s_synmap_inflight &&
            e->catalog_seq == s_synmap_seq) {
            bool printed = nanocore_protocol_print_synth_capture(s_synmap_seq);
            s_synmap_inflight = false;
            ESP_LOGI(TAG, "SYNMAP|MAIN_STACK_FREE|bytes=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (!printed) ESP_LOGW(TAG, "SYNMAP|MAILBOX_MISSING|seq=%u", s_synmap_seq);
            show_notice(printed ? "SYNTH CAPTURE OK" : "CAPTURE LOST");
        }
        return;
    }
    /* Physical changes during discovery are evidence, not local edits.
     * In particular do not trigger Scene overlays, recall or NVS writes. */
    if (s_synmap_enabled) return;

    if (e->type == NC_PROTO_EVENT_MODEL_CATALOG) {
        handle_catalog_event(e);
        return;
    }
    catalog_defer(e->type == NC_PROTO_EVENT_LIVE_STATE ||
                  e->type == NC_PROTO_EVENT_PRESET_NAME
                      ? CATALOG_IDLE_MS : CATALOG_EVENT_GAP_MS);

    switch (e->type) {
    case NC_PROTO_EVENT_GLOBAL_LEVELS:
        if(!e->global_push && s_levels_wait && e->global_seq==s_levels_seq) {
            s_levels_wait=false;
            s_levels_known=true;
            s_levels_write_ready=true;
            s_input_gain_db=e->input_gain_db;
            if(s_levels_write_pending && s_input_gain_db!=s_levels_target_db) {
                s_levels_edit=false;
                s_levels_write_ready=false;
                ESP_LOGW(TAG,"LEVEL|WRITE_MISMATCH|wanted=%d|read=%d",(int)s_levels_target_db,(int)s_input_gain_db);
                queue_notice("INPUT GAIN NOT CONFIRMED");
            } else ESP_LOGI(TAG,"LEVEL|CONFIRMED|input_db=%d",(int)s_input_gain_db);
            s_levels_write_pending=false;
        } else if(e->global_push && !s_levels_wait) {
            s_levels_known=true;
            s_input_gain_db=e->input_gain_db;
        }
        if(s_screen==UI_SCREEN_LEVELS) render_ui();
        break;
    case NC_PROTO_EVENT_LIVE_STATE: {
        bool was_recall = s_recall_pending;
        bool probe=s_slot_probe_inflight && e->live_seq==s_slot_probe_seq;
        if(probe) {
            s_slot_probe_inflight=false;
            if(scene_busy() || s_recall_pending || e->slot>=CTRL_PRESET_COUNT ||
               !e->chain_order_known || !e->synth_mode_known) break;
            if(e->slot!=s_runtime.current_preset) {
                uint8_t previous=s_runtime.current_preset;
                bool had_dirty=s_dirty;
                if(load_preset(e->slot,false)!=ESP_OK) {
                    show_notice("PRESET READ FAILED");
                    break;
                }
                nanocore_protocol_set_level_diagnostics(false);
                if(s_name_editor.active) name_editor_close(&s_name_editor);
                clear_block_state();
                was_recall=true;
                s_name_after_live=true;
                ESP_LOGI(TAG,"SLOT|CONFIRMED|from=%u|to=%u",previous,e->slot);
                queue_notice(had_dirty?"NANOCORE CHANGED - LOCAL EDIT LOST":"NANOCORE PRESET CHANGED");
            } else {
                ESP_LOGI(TAG,"SLOT|UNCHANGED|slot=%u",e->slot);
            }
        }
        if (s_scene_phase != SCENE_IDLE &&
            (e->live_seq != s_scene_seq || e->slot != s_runtime.current_preset))
            break;
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

        bool level_known=e->live_header_byte<=100;
        if(level_known && (!s_level_raw_known || s_level_raw!=e->live_header_byte))
            ESP_LOGI(TAG,"LEVEL|PRESET_VOLUME|slot=%u|value=%u",e->slot,e->live_header_byte);
        s_level_raw_known=level_known;
        s_level_raw=e->live_header_byte;
        if (e->synth_mode_known) {
            bool changed = s_synth_mode != e->synth_mode;
            if (changed && !was_recall && s_scene_phase == SCENE_IDLE && s_synth_mode_known) {
                sound_edit_begin();
                sound_edit_confirm(e);
                s_scene_verified_models = false;
            }
            s_synth_mode_known = true;
            s_synth_mode = e->synth_mode;
            s_synth_voice_known = e->synth_known;
            s_synth_voice_on = e->synth_on;
            s_synth_voice = e->synth_variant;
            s_synth_param_count = e->synth_param_count;
            memcpy(s_synth_params, e->synth_params, sizeof(s_synth_params));
            if (changed) {
                s_synth_selected = s_synth_mode;
                s_levels_selected=false;
                s_selected_block = BLOCK_FX1;
                s_selected_parameter = 0;
                s_parameter_editing = false;
                if (s_scene_phase != SCENE_BASE_SAVE && s_scene_phase != SCENE_SAVE)
                    s_screen = UI_SCREEN_PERFORMANCE;
                if (s_synth_mode && tempo_engine_tap_active()) {
                    tempo_engine_cancel_tap();
                    status_leds_set_tap_capture(false);
                }
                ESP_LOGI(TAG, "SYNTH|UI|mode=%u", s_synth_mode);
            }
        } else {
            s_synth_mode_known = false;
            s_synth_voice_known = false;
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

        /*
         * A physical type change may shrink the editor parameter list.
         * Never leave the cursor pointing beyond the newly selected type.
         */
        if (s_screen == UI_SCREEN_BLOCK_EDITOR) {
            size_t count = editor_parameter_count();
            if (count > 0 && s_selected_parameter >= count) {
                s_selected_parameter = count - 1;
                s_parameter_editing = false;
            }
        }

        // Assisted Mapper can still consume the accepted LIVE snapshot.
        nanocore_mapper_on_live_state(e);

        /*
         * AMP/CAB model identity is not present in 0x63. If their editor
         * requested a fresh snapshot, follow it with one saved-footer read.
         * During normal recall the name flow below will request models after
         * the 0x41 name reply, avoiding overlapping 0x41 transactions.
         */
        if (s_editor_models_after_live && !s_name_after_live) {
            s_editor_models_after_live = false;
            s_models_read_retries=0;
            (void)nanocore_protocol_request_preset_models(
                s_runtime.current_preset);
        }

        /* Avoid a complete TFT redraw before sending scene commands. */
        if (s_scene_phase != SCENE_IDLE) {
            scene_phase_t completed = s_scene_phase;
            scene_handle_live(e);
            if (completed == SCENE_BASE_RESTORE && s_scene_phase == SCENE_IDLE &&
                !s_scene_fault && s_name_after_live) {
                if(s_runtime.mode==CTRL_MODE_SCENE) {
                    s_scene_after_models=true;
                    s_scene_base_deadline=esp_timer_get_time()+5000000LL;
                }
                s_name_after_live = false;
                (void)nanocore_protocol_request_preset_name(s_runtime.current_preset);
            }
            break;
        }

        if (was_recall && e->synth_mode_known) {
            if (!s_base_mode_known) {
                s_base_mode_known = true;
                s_base_mode = e->synth_mode;
            }
            if (s_base_mode != e->synth_mode) {
                s_scene_target_mode = s_base_mode;
                s_scene_read_retries = 0;
                s_scene_phase = SCENE_BASE_RESTORE;
                if (nanocore_protocol_set_synth_mode(s_base_mode) == ESP_OK) {
                    queue_notice("RESTORING PRESET MODE");
                    (void)scene_start_read(SCENE_BASE_RESTORE);
                } else scene_fail("PRESET MODE TX FAILED");
                break;
            }
        }
        render_ui();

        if (was_recall && s_runtime.mode == CTRL_MODE_SCENE) {
            /* Wait for saved model IDs before applying the new scene;
             * later 0x41 replies must not overwrite scene model mirrors. */
            s_scene_after_models = true;
            s_scene_base_deadline = esp_timer_get_time() + 5000000LL;
        }

        // Mapper polls must not trigger continuous 0x41/name traffic.
        // Read the name only after recall/save explicitly asked for it.
        if (s_name_after_live) {
            s_name_after_live = false;
            (void)nanocore_protocol_request_preset_name(
                s_runtime.current_preset);
        }
        break;
    }

    case NC_PROTO_EVENT_BLOCK_STATE: {
        if (e->block >= BLOCK_COUNT || scene_busy() || s_recall_pending) break;

        bool changed = !s_block_known[e->block] || s_block_on[e->block] != e->on;
        s_block_known[e->block] = true;
        s_block_on[e->block] = e->on;
        if (s_runtime.mode == CTRL_MODE_SCENE || s_synth_mode) {
            if (changed) {
                mark_edit_dirty();
                s_scene_verified_models = false;
            }
            render_ui();
            break;
        }

        /*
         * A physical NanoCore block toggle is a real unsaved live edit.
         * Mirror it in the current controller context so SAVE preserves the
         * same state in NVS as the pedal.
         */
        if(!changed) break; /* Local command echoes are not new edits. */
        size_t count = 0;
        midi_assignment_t *items = edit_assignments(&count);
        (void)controller_assignment_set(
            items, count,
            nanocore_block_onoff_cc(e->block),
            e->on ? 127 : 0);
        mark_edit_dirty();
        render_ui();
        break;
    }

    case NC_PROTO_EVENT_ACTIVE_SLOT:
        if(!scene_busy() && !s_recall_pending && !s_recall_poll_inflight)
            slot_probe_request(false);
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

            /*
             * The preset-name request is serialized after accepted 0x63.
             * Reuse that completed 0x41 transaction as the trigger for the
             * separate footer read that supplies AMP/CAB model ids.
             */
            if (s_midi_ready) {
                s_models_read_retries=0;
                (void)nanocore_protocol_request_preset_models(
                    s_runtime.current_preset);
            }
        }
        break;

    case NC_PROTO_EVENT_PRESET_MODELS: {
        if (e->slot != s_runtime.current_preset || s_scene_phase != SCENE_IDLE) break;

        /* Variable-size saved presets (including Synth) can place the
         * footer outside the legacy fixed window. Two bounded reads. */
        if (!e->amp_model_known && !e->cab_model_known &&
            s_models_read_retries < 2) {
            uint16_t tail=e->preset_window_total>59 ? e->preset_window_total-59 : 1;
            uint16_t next=s_models_read_retries==0 ? tail : 180;
            if(next==e->preset_window_offset || next==220) next=180;
            if(next!=e->preset_window_offset && next<=1024) {
                ++s_models_read_retries;
                ESP_LOGI(TAG,"MODEL|WINDOW_RETRY|offset=%u",next);
                if(nanocore_protocol_request_preset_models_window(e->slot,next)==ESP_OK)
                    break;
            }
        }

        if (e->amp_model_known && !s_amp_model_known) {
            s_amp_model = e->amp_model;
            s_amp_model_known = true;
        }
        if (e->cab_model_known && !s_cab_model_known) {
            s_cab_model = e->cab_model;
            s_cab_model_known = true;
        }
        s_editor_models_after_live = false;

        char amp_text[48], cab_text[48];
        format_model_name(BLOCK_AMP, e->amp_model, amp_text, sizeof(amp_text));
        format_model_name(BLOCK_CAB, e->cab_model, cab_text, sizeof(cab_text));
        const char *amp_name = e->amp_model_known ? amp_text : NULL;
        const char *cab_name = e->cab_model_known ? cab_text : NULL;

        ESP_LOGI(TAG,
                 "MODEL|slot=%u|AMP=%s(%u)|CAB=%s(%u)",
                 e->slot,
                 amp_name ? amp_name : "UNKNOWN", e->amp_model,
                 cab_name ? cab_name : "UNKNOWN", e->cab_model);

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

        if (s_scene_after_models) {
            s_scene_after_models = false;
            /* Full scene has its own model IDs even if saved-preset
             * footer is missing. No timeout just for an unknown base ID. */
            apply_current_scene();
        }
        render_ui();
        break;
    }

    case NC_PROTO_EVENT_SYNTH_PARAM_VALUE:
        if (!scene_busy() && s_synth_mode && s_synth_voice_known &&
            e->param_index < s_synth_param_count) {
            bool changed = fabsf(s_synth_params[e->param_index] - e->param_value) > 0.00001f;
            s_synth_params[e->param_index] = e->param_value;
            if (changed) {
                mark_edit_dirty();
                s_scene_verified_models = false;
            }
            render_ui();
        }
        break;

    case NC_PROTO_EVENT_PARAM_VALUE:
        if (scene_busy()) break;
        if (e->block < BLOCK_COUNT &&
            e->param_index < NC_PROTO_MAX_PARAMS) {
            bool changed = !s_block_profile_known[e->block] ||
                e->param_index >= s_block_param_count[e->block] ||
                fabsf(s_block_params[e->block][e->param_index] - e->param_value) > 0.00001f;
            s_block_profile_known[e->block] = true;
            if (s_block_param_count[e->block] <= e->param_index) {
                s_block_param_count[e->block] =
                    (uint8_t)(e->param_index + 1);
            }
            s_block_params[e->block][e->param_index] =
                e->param_value;

            /*
             * 0x06 is emitted by a physical panel knob turn; MIDI/SysEx
             * writes do not echo it. Therefore this is a genuine unsaved
             * device-side edit and must participate in the dirty guard.
             */
            if (changed) {
                mark_edit_dirty();
                s_scene_verified_models = false;
            }
            render_ui();
        }
        break;

    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " NANOCORE MIDI CONTROLLER - STAGE 10B / PRESET VOLUME READ + INPUT GUARD");
    ESP_LOGI(TAG, " ESP32-S3 N16R8 + ST7796S 3.5");
    ESP_LOGI(TAG, "==============================================");

    nvs_init_safe();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(controller_storage_init());

    ESP_ERROR_CHECK(controller_storage_load_runtime(&s_runtime));
    ESP_ERROR_CHECK(controller_storage_load_preset(
        s_runtime.current_preset,
        &s_preset));
    scene_load_names();

    if (s_runtime.current_scene >= CTRL_SCENE_COUNT) {
        s_runtime.current_scene = 0;
    }

    tempo_engine_init(s_preset.bpm);

    // Bring up the display before background input/LED tasks so a fault in
    // those subsystems cannot hide boot diagnostics or leave the TFT dark.
    ESP_ERROR_CHECK(display_ui_init());
    (void)battery_manager_init(); /* Optional hardware: failure must not stop controller. */
    render_ui();

    ESP_ERROR_CHECK(status_leds_init(s_runtime.mode, s_preset.bpm));
    ESP_ERROR_CHECK(input_manager_init());
    ESP_ERROR_CHECK(nanocore_protocol_init());
    ESP_ERROR_CHECK(nanocore_mapper_init());

    ble_init();

    bool last_tap_active = tempo_engine_tap_active();

    while (1) {
        catalog_reset_if_needed();
        if (s_scene_after_models && s_midi_ready &&
            esp_timer_get_time() >= s_scene_base_deadline) {
            s_scene_after_models=false;
            ESP_LOGW(TAG,"MODEL|BASE_READ_TIMEOUT|using scene snapshot if present");
            apply_current_scene();
        } else if (scene_busy() &&
            (!s_midi_ready ||
             (s_scene_phase != SCENE_IDLE && esp_timer_get_time() >= s_scene_deadline))) {
            scene_fail("SCENE SYNC TIMEOUT");
        }
        if (s_synmap_enabled && !s_midi_ready) {
            s_synmap_enabled = false;
            s_synmap_inflight = false;
            ESP_LOGW(TAG, "SYNMAP|ABORT|BLE disconnected");
        }
        if (s_synmap_enabled && s_synmap_inflight &&
            esp_timer_get_time() - s_synmap_sent_us >= SYNMAP_TIMEOUT_MS * 1000LL) {
            s_synmap_inflight = false;
            ESP_LOGW(TAG, "SYNMAP|TIMEOUT|seq=%u", s_synmap_seq);
            show_notice("CAPTURE TIMEOUT");
        }
        /*
         * Rotation is coalesced: one fast physical turn may arrive as +20
         * detents, which updates the model once and renders once instead of
         * replaying 20 stale screen frames.
         */
        int enc1 = input_manager_take_encoder_delta(1);
        int enc2 = input_manager_take_encoder_delta(2);
        if (enc1 || enc2) catalog_defer(CATALOG_IDLE_MS);
        if (enc1) handle_encoder_delta(1, enc1);
        if (enc2) handle_encoder_delta(2, enc2);

        input_event_t e = INPUT_NONE;
        TickType_t input_wait = pdMS_TO_TICKS(10);
        if (input_wait < 1) input_wait = 1;
        if (input_manager_get_event(&e, input_wait)) {
            catalog_defer(CATALOG_IDLE_MS);
            handle_input(e);
        }

        /*
         * Live protocol events carry all block float parameters and are
         * intentionally large. Keep the receive buffer out of app_main's
         * task stack as well.
         */
        static nanocore_protocol_event_t pe;
        while (nanocore_protocol_get_event(&pe, 0)) {
            handle_protocol_event(&pe);
        }

        service_recall_sync();
        service_slot_probe();
        if(s_discard_action!=DISCARD_NONE && !s_midi_ready) {
            discard_cancel();
            render_ui();
        }
        if (s_pending_scene_delta && !s_catalog_inflight && !scene_busy() &&
            !s_slot_probe_pending && !s_slot_probe_inflight && !s_levels_wait) {
            int delta=s_pending_scene_delta;
            s_pending_scene_delta=0;
            if (s_runtime.mode==CTRL_MODE_SCENE && s_midi_ready && !s_recall_pending)
                change_scene(delta);
        }
        service_model_catalog();
        if (!scene_busy() && !s_recall_pending && !s_catalog_inflight && !s_synmap_enabled) {
            bool known;
            uint8_t percent;
            if (battery_manager_poll(&known, &percent))
                display_ui_update_battery(known, percent);
        }
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
