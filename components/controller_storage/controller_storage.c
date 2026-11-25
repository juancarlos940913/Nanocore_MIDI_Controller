#include "controller_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORAGE_NS       "nc_ctrl2"
#define STORAGE_VERSION   2
#define SCENE_PARTITION "scene_data"
#define SCENE_NAMESPACE "nc_scene1"
static bool s_scene_storage_ready;

static const char *TAG = "NC_STORAGE";

static void runtime_defaults(controller_runtime_t *r)
{
    memset(r, 0, sizeof(*r));
    r->version = STORAGE_VERSION;
    r->current_preset = 0;
    r->current_scene = 0;
    r->mode = CTRL_MODE_PRESET;
}

void controller_storage_default_preset(uint8_t index,
                                       controller_preset_t *p)
{
    if (!p) return;

    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "PRESET %02u",
             (unsigned)(index + 1));
    p->bpm = 120;
    p->delay_subdivision = DELAY_SUBDIV_QUARTER;
    p->active_scene = 0;

    for (uint8_t i = 0; i < CTRL_SCENE_COUNT; ++i) {
        snprintf(p->scene[i].name,
                 sizeof(p->scene[i].name),
                 "SCENE %c",
                 (char)('A' + i));
    }
}

esp_err_t controller_storage_init(void)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(STORAGE_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    nvs_close(h);
    ESP_LOGI(TAG, "NVS storage ready");
    /* Do not erase this partition on init errors. Legacy storage remains
     * usable even if a board still has the old partition table. */
    rc = nvs_flash_init_partition(SCENE_PARTITION);
    s_scene_storage_ready = rc == ESP_OK;
    ESP_LOGI(TAG, "SCENE|STORAGE|%s", esp_err_to_name(rc));
    return ESP_OK;
}

esp_err_t controller_storage_load_runtime(controller_runtime_t *runtime)
{
    if (!runtime) return ESP_ERR_INVALID_ARG;

    runtime_defaults(runtime);

    nvs_handle_t h;
    esp_err_t rc = nvs_open(STORAGE_NS, NVS_READONLY, &h);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;

    size_t len = sizeof(*runtime);
    rc = nvs_get_blob(h, "runtime", runtime, &len);
    nvs_close(h);

    if (rc == ESP_ERR_NVS_NOT_FOUND ||
        len != sizeof(*runtime) ||
        runtime->version != STORAGE_VERSION ||
        runtime->current_preset >= CTRL_PRESET_COUNT ||
        runtime->current_scene >= CTRL_SCENE_COUNT ||
        runtime->mode >= CTRL_MODE_COUNT) {
        runtime_defaults(runtime);
        return ESP_OK;
    }

    return rc;
}

esp_err_t controller_storage_save_runtime(const controller_runtime_t *runtime)
{
    if (!runtime) return ESP_ERR_INVALID_ARG;

    controller_runtime_t copy = *runtime;
    copy.version = STORAGE_VERSION;

    nvs_handle_t h;
    esp_err_t rc = nvs_open(STORAGE_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;

    rc = nvs_set_blob(h, "runtime", &copy, sizeof(copy));
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    return rc;
}

static void preset_key(uint8_t index, char key[8])
{
    snprintf(key, 8, "p%02u", (unsigned)index);
}

esp_err_t controller_storage_load_preset(uint8_t index,
                                         controller_preset_t *preset)
{
    if (!preset || index >= CTRL_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    controller_storage_default_preset(index, preset);

    nvs_handle_t h;
    esp_err_t rc = nvs_open(STORAGE_NS, NVS_READONLY, &h);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc != ESP_OK) return rc;

    char key[8];
    preset_key(index, key);

    size_t len = sizeof(*preset);
    rc = nvs_get_blob(h, key, preset, &len);
    nvs_close(h);

    if (rc == ESP_ERR_NVS_NOT_FOUND || len != sizeof(*preset)) {
        controller_storage_default_preset(index, preset);
        return ESP_OK;
    }

    if (preset->bpm < 30 || preset->bpm > 300) {
        preset->bpm = 120;
    }
    if (preset->active_scene >= CTRL_SCENE_COUNT) {
        preset->active_scene = 0;
    }
    if (preset->delay_subdivision >= DELAY_SUBDIV_COUNT) {
        preset->delay_subdivision = DELAY_SUBDIV_QUARTER;
    }

    return rc;
}

esp_err_t controller_storage_save_preset(uint8_t index,
                                         const controller_preset_t *preset)
{
    if (!preset || index >= CTRL_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t rc = nvs_open(STORAGE_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;

    char key[8];
    preset_key(index, key);

    rc = nvs_set_blob(h, key, preset, sizeof(*preset));
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Preset %u persisted (%u bytes)",
                 index, (unsigned)sizeof(*preset));
    }
    return rc;
}

bool controller_assignment_get(const midi_assignment_t *items,
                               size_t count,
                               uint8_t cc,
                               uint8_t *value)
{
    if (!items) return false;

    for (size_t i = 0; i < count; ++i) {
        if (items[i].used && items[i].cc == cc) {
            if (value) *value = items[i].value;
            return true;
        }
    }
    return false;
}

esp_err_t controller_assignment_set(midi_assignment_t *items,
                                    size_t count,
                                    uint8_t cc,
                                    uint8_t value)
{
    if (!items || cc > 127 || value > 127) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; ++i) {
        if (items[i].used && items[i].cc == cc) {
            items[i].value = value;
            return ESP_OK;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        if (!items[i].used) {
            items[i].used = 1;
            items[i].cc = cc;
            items[i].value = value;
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

static void scene_key(uint8_t preset, uint8_t scene, char key[12])
{
    snprintf(key, 12, "p%02u_s%u", (unsigned)preset, (unsigned)scene);
}

esp_err_t controller_storage_scene_load(uint8_t preset, uint8_t scene,
                                        controller_scene_profile_t *profile)
{
    if (!profile || preset >= CTRL_PRESET_COUNT || scene >= CTRL_SCENE_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (!s_scene_storage_ready) return ESP_ERR_INVALID_STATE;
    memset(profile, 0, sizeof(*profile));
    nvs_handle_t h;
    esp_err_t rc = nvs_open_from_partition(SCENE_PARTITION, SCENE_NAMESPACE, NVS_READONLY, &h);
    if (rc != ESP_OK) return rc;
    char key[12];
    scene_key(preset, scene, key);
    size_t len = sizeof(*profile);
    rc = nvs_get_blob(h, key, profile, &len);
    nvs_close(h);
    if (rc != ESP_OK) return rc;
    if (len != sizeof(*profile) || profile->version != CTRL_PROFILE_VERSION ||
        profile->synth_mode > 1 || profile->synth_on > 1)
        return ESP_ERR_INVALID_SIZE;
    profile->name[CTRL_NAME_LEN - 1] = '\0';
    return ESP_OK;
}

esp_err_t controller_storage_scene_save(uint8_t preset, uint8_t scene,
                                        const controller_scene_profile_t *profile)
{
    if (!profile || preset >= CTRL_PRESET_COUNT || scene >= CTRL_SCENE_COUNT ||
        profile->version != CTRL_PROFILE_VERSION) return ESP_ERR_INVALID_ARG;
    if (!s_scene_storage_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t rc = nvs_open_from_partition(SCENE_PARTITION, SCENE_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    char key[12];
    scene_key(preset, scene, key);
    rc = nvs_set_blob(h, key, profile, sizeof(*profile));
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    return rc;
}

esp_err_t controller_storage_base_mode_load(uint8_t preset, bool *synth)
{
    if (!synth || preset >= CTRL_PRESET_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_scene_storage_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t rc=nvs_open_from_partition(SCENE_PARTITION,SCENE_NAMESPACE,NVS_READONLY,&h);
    if (rc!=ESP_OK) return rc;
    char key[12];
    snprintf(key,sizeof(key),"p%02u_mode",(unsigned)preset);
    uint8_t v=0;
    rc=nvs_get_u8(h,key,&v);
    nvs_close(h);
    if (rc!=ESP_OK) return rc;
    if (v>1) return ESP_ERR_INVALID_ARG;
    *synth=v!=0;
    return ESP_OK;
}

esp_err_t controller_storage_base_mode_save(uint8_t preset, bool synth)
{
    if (preset >= CTRL_PRESET_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_scene_storage_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t rc=nvs_open_from_partition(SCENE_PARTITION,SCENE_NAMESPACE,NVS_READWRITE,&h);
    if (rc!=ESP_OK) return rc;
    char key[12];
    snprintf(key,sizeof(key),"p%02u_mode",(unsigned)preset);
    rc=nvs_set_u8(h,key,synth?1:0);
    if (rc==ESP_OK) rc=nvs_commit(h);
    nvs_close(h);
    return rc;
}
