/*
 * Snapshot persistence - Stage 04.3
 *
 * For now a snapshot owns only the Amp Type selection.
 * More optional fields (gain/EQ/mod/delay/reverb/etc.) can be added later
 * without changing the performance-mode contract.
 */

#include "snapshot_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "NC_SNAPSHOT";
static const char *NVS_NS = "nc_snap";

static snapshot_t s_snapshots[SNAPSHOT_COUNT];

static void key_for_amp(uint8_t index, char key[8])
{
    snprintf(key, 8, "amp_%u", (unsigned)index);
}

esp_err_t snapshot_manager_init(void)
{
    memset(s_snapshots, 0, sizeof(s_snapshots));

    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READONLY, &h);

    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved snapshots yet");
        return ESP_OK;
    }
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(rc));
        return rc;
    }

    for (uint8_t i = 0; i < SNAPSHOT_COUNT; ++i) {
        char key[8];
        key_for_amp(i, key);

        uint8_t value = 0;
        rc = nvs_get_u8(h, key, &value);

        if (rc == ESP_OK) {
            s_snapshots[i].amp_configured = true;
            s_snapshots[i].amp_type_id = value & 0x7F;
            ESP_LOGI(TAG, "Scene %c amp_type=%u loaded",
                     (char)('A' + i), s_snapshots[i].amp_type_id);
        } else if (rc != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Could not read %s: %s",
                     key, esp_err_to_name(rc));
        }
    }

    nvs_close(h);
    return ESP_OK;
}

const snapshot_t *snapshot_manager_get(uint8_t index)
{
    if (index >= SNAPSHOT_COUNT) {
        return NULL;
    }
    return &s_snapshots[index];
}

esp_err_t snapshot_manager_set_amp(uint8_t index, uint8_t amp_type_id)
{
    if (index >= SNAPSHOT_COUNT || amp_type_id > 127) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        return rc;
    }

    char key[8];
    key_for_amp(index, key);

    rc = nvs_set_u8(h, key, amp_type_id);
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);

    if (rc == ESP_OK) {
        s_snapshots[index].amp_configured = true;
        s_snapshots[index].amp_type_id = amp_type_id;
        ESP_LOGI(TAG, "Scene %c amp_type=%u saved",
                 (char)('A' + index), amp_type_id);
    }

    return rc;
}

esp_err_t snapshot_manager_clear_amp(uint8_t index)
{
    if (index >= SNAPSHOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        return rc;
    }

    char key[8];
    key_for_amp(index, key);

    rc = nvs_erase_key(h, key);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        rc = ESP_OK;
    }
    if (rc == ESP_OK) {
        rc = nvs_commit(h);
    }
    nvs_close(h);

    if (rc == ESP_OK) {
        s_snapshots[index].amp_configured = false;
        s_snapshots[index].amp_type_id = 0;
    }

    return rc;
}
