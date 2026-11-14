/*
 * Nanocore MIDI Controller
 * Stage 01 - BLE Scan
 *
 * Goal:
 *   - Bring up ESP32-S3 as BLE Central using ESP-NimBLE.
 *   - Perform active BLE scanning.
 *   - Log advertiser name, address, RSSI and whether the standard
 *     BLE-MIDI service UUID is present.
 *   - DO NOT connect yet.
 *
 * Expected Nanocore name from the user manual: "Nanocore Midi".
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_conn_mgr.h"
#include "esp_ble_midi_svc.h"

static const char *TAG = "NC_STAGE01";

static const uint8_t MIDI_SERVICE_UUID[16] = BLE_MIDI_SERVICE_UUID128;

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
    if (!out || out_len == 0) {
        return;
    }

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
        if (copy_len >= out_len) {
            copy_len = out_len - 1;
        }
        memcpy(out, name, copy_len);
        out[copy_len] = '\0';
    }
}

static bool contains_nanocore(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }

    char lower[48] = {0};
    size_t n = strlen(name);
    if (n >= sizeof(lower)) {
        n = sizeof(lower) - 1;
    }

    for (size_t i = 0; i < n; ++i) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }

    return strstr(lower, "nanocore") != NULL;
}

static bool scan_cb(const esp_ble_conn_scan_result_t *result, void *arg)
{
    (void)arg;

    if (!result) {
        return false;
    }

    char name[48];
    adv_get_name(result->adv_data, result->adv_data_len, name, sizeof(name));

    const bool midi_uuid = adv_has_midi_uuid(result->adv_data,
                                             result->adv_data_len);
    const bool nanocore_name = contains_nanocore(name);

    ESP_LOGI(TAG,
             "BLE addr=" BLE_CONN_MGR_ADDR_STR
             " RSSI=%d name=\"%s\" MIDI_UUID=%s%s",
             BLE_CONN_MGR_ADDR_HEX(result->addr),
             result->rssi,
             name[0] ? name : "<no-name>",
             midi_uuid ? "YES" : "no",
             nanocore_name ? "  <== NANOCORE candidate" : "");

    // Stage 01 is scan-only. Returning false prevents auto-connect.
    return false;
}

static void ble_event_handler(void *handler_args,
                              esp_event_base_t base,
                              int32_t id,
                              void *event_data)
{
    (void)handler_args;
    (void)event_data;

    if (base != BLE_CONN_MGR_EVENTS) {
        return;
    }

    switch (id) {
    case ESP_BLE_CONN_EVENT_STARTED:
        ESP_LOGI(TAG, "BLE manager started");
        break;

    case ESP_BLE_CONN_EVENT_CONNECTED:
        ESP_LOGW(TAG, "Unexpected connection in Stage 01");
        break;

    case ESP_BLE_CONN_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        break;

    default:
        break;
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
    ESP_LOGI(TAG, " NANOCORE MIDI CONTROLLER - STAGE 01");
    ESP_LOGI(TAG, " BLE SCAN / DISCOVERY");
    ESP_LOGI(TAG, "======================================");

    nvs_init_safe();

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

    // Active scan is important because some devices put their name in
    // the scan response instead of the primary advertising packet.
    esp_ble_conn_scan_params_t scan_params = {
        .passive = false,
        .filter_duplicates = true,
    };
    ESP_ERROR_CHECK(esp_ble_conn_scan_params_set(&scan_params));

    ESP_ERROR_CHECK(esp_ble_conn_start());

    ESP_LOGI(TAG, "Scanning...");
    ESP_LOGI(TAG, "Look for 'Nanocore Midi' and/or MIDI_UUID=YES.");
    ESP_LOGI(TAG, "Stage 01 never connects; copy the serial log after test.");
}
