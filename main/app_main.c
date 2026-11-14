/*
 * Nanocore MIDI Controller
 * Stage 03 - BLE-MIDI Program Change
 *
 * Hardware test controls:
 *   BTN_PROGRAM  GPIO18 -> previous preset
 *   BTN_STOP     GPIO21 -> next preset
 *   BTN_FUNCTION GPIO47 -> resend current preset
 *
 * For the first test, set NANOCORE MIDI Channel = 0 (Omni).
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
#include "nanocore_midi.h"

static const char *TAG = "NC_STAGE03";

static const uint8_t MIDI_SERVICE_UUID[16] = BLE_MIDI_SERVICE_UUID128;
static const uint8_t MIDI_CHAR_UUID[16]    = BLE_MIDI_CHAR_UUID128;

#define NANOCORE_MIDI_CHANNEL       1
#define NANOCORE_PRESET_MIN         0
#define NANOCORE_PRESET_MAX        63

#define BUTTON_POLL_MS             10
#define BUTTON_DEBOUNCE_MS         40

static bool s_target_found = false;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint16_t s_midi_char_handle = 0;
static uint8_t s_program = 0;

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

    if (!result || s_target_found) {
        return false;
    }

    char name[48];
    adv_get_name(result->adv_data, result->adv_data_len, name, sizeof(name));

    const bool midi_uuid = adv_has_midi_uuid(result->adv_data,
                                             result->adv_data_len);
    const bool nanocore_name = contains_nanocore(name);

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
    if (!d || d->type != BLE_CONN_UUID_TYPE_128) {
        return false;
    }

    return memcmp(d->uuid.uuid128,
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
        ESP_LOGE(TAG,
                 "BLE-MIDI characteristic NOT found: %s",
                 esp_err_to_name(rc));
        return false;
    }

    ESP_LOGI(TAG,
             "BLE-MIDI characteristic found: handle=0x%04X",
             s_midi_char_handle);

    return true;
}

static void ble_event_handler(void *handler_args,
                              esp_event_base_t base,
                              int32_t id,
                              void *event_data)
{
    (void)handler_args;

    if (base != BLE_CONN_MGR_EVENTS) {
        return;
    }

    switch (id) {
    case ESP_BLE_CONN_EVENT_STARTED:
        ESP_LOGI(TAG, "BLE manager started; scanning...");
        break;

    case ESP_BLE_CONN_EVENT_CONNECTED:
        if (esp_ble_conn_get_conn_handle(&s_conn_handle) != ESP_OK ||
            s_conn_handle == BLE_CONN_HANDLE_INVALID) {
            ESP_LOGE(TAG, "Connected event but no valid connection handle");
            break;
        }

        ESP_LOGI(TAG, "CONNECTED: conn_handle=%u", s_conn_handle);

        {
            esp_err_t mtu_rc = esp_ble_conn_mtu_update(s_conn_handle, 185);
            if (mtu_rc != ESP_OK) {
                ESP_LOGW(TAG, "MTU request failed: %s",
                         esp_err_to_name(mtu_rc));
            }
        }

        {
            esp_err_t sec_rc =
                esp_ble_conn_security_initiate(s_conn_handle);
            if (sec_rc == ESP_OK) {
                ESP_LOGI(TAG, "BLE security/pairing initiated");
            } else {
                ESP_LOGW(TAG, "Security initiate returned: %s",
                         esp_err_to_name(sec_rc));
            }
        }
        break;

    case ESP_BLE_CONN_EVENT_MTU: {
        esp_ble_conn_event_data_t *evt =
            (esp_ble_conn_event_data_t *)event_data;

        if (evt) {
            ESP_LOGI(TAG, "MTU negotiated: %u",
                     evt->mtu_update.mtu);
        }
        break;
    }

    case ESP_BLE_CONN_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "GATT discovery complete");

        if (s_conn_handle != BLE_CONN_HANDLE_INVALID &&
            resolve_peer_midi_handle(s_conn_handle)) {

            subscribe_midi_notifications(s_conn_handle);

            nanocore_midi_set_link(
                s_conn_handle,
                s_midi_char_handle);

            ESP_LOGI(TAG, "======================================");
            ESP_LOGI(TAG, " BLE-MIDI READY");
            ESP_LOGI(TAG, " GPIO18 = PREVIOUS PRESET");
            ESP_LOGI(TAG, " GPIO21 = NEXT PRESET");
            ESP_LOGI(TAG, " GPIO47 = RESEND CURRENT PRESET");
            ESP_LOGI(TAG, " Current PC = %u", s_program);
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

        if (esp_ble_conn_scan_start() != ESP_OK) {
            ESP_LOGW(TAG, "Could not restart scan");
        }
        break;

    default:
        break;
    }
}

static void send_current_program(void)
{
    if (!nanocore_midi_is_ready()) {
        ESP_LOGW(TAG,
                 "MIDI not ready; PC %u not sent",
                 s_program);
        return;
    }

    ESP_LOGI(TAG, "Request PC -> %u", s_program);

    esp_err_t rc = nanocore_midi_send_program_change(
        NANOCORE_MIDI_CHANNEL,
        s_program);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG,
                 "Program Change failed: %s",
                 esp_err_to_name(rc));
    }
}

typedef struct {
    gpio_num_t gpio;
    bool stable_pressed;
    bool last_raw_pressed;
    TickType_t last_change_tick;
} button_state_t;

static bool button_update(button_state_t *btn, TickType_t now)
{
    // Existing board buttons are treated as active LOW.
    const bool raw_pressed = gpio_get_level(btn->gpio) == 0;

    if (raw_pressed != btn->last_raw_pressed) {
        btn->last_raw_pressed = raw_pressed;
        btn->last_change_tick = now;
    }

    const TickType_t debounce_ticks =
        pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    if ((now - btn->last_change_tick) >= debounce_ticks &&
        btn->stable_pressed != raw_pressed) {

        btn->stable_pressed = raw_pressed;

        // Return true only on the debounced press edge.
        if (btn->stable_pressed) {
            return true;
        }
    }

    return false;
}

static void buttons_init(void)
{
    const uint64_t pin_mask =
        (1ULL << BTN_PROGRAM) |
        (1ULL << BTN_STOP) |
        (1ULL << BTN_FUNCTION);

    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
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

    button_state_t prev = {
        .gpio = BTN_PROGRAM,
        .stable_pressed = false,
        .last_raw_pressed = false,
        .last_change_tick = xTaskGetTickCount(),
    };

    button_state_t next = {
        .gpio = BTN_STOP,
        .stable_pressed = false,
        .last_raw_pressed = false,
        .last_change_tick = xTaskGetTickCount(),
    };

    button_state_t resend = {
        .gpio = BTN_FUNCTION,
        .stable_pressed = false,
        .last_raw_pressed = false,
        .last_change_tick = xTaskGetTickCount(),
    };

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (button_update(&prev, now)) {
            if (s_program <= NANOCORE_PRESET_MIN) {
                s_program = NANOCORE_PRESET_MAX;
            } else {
                --s_program;
            }

            send_current_program();
        }

        if (button_update(&next, now)) {
            if (s_program >= NANOCORE_PRESET_MAX) {
                s_program = NANOCORE_PRESET_MIN;
            } else {
                ++s_program;
            }

            send_current_program();
        }

        if (button_update(&resend, now)) {
            send_current_program();
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
    ESP_LOGI(TAG, " NANOCORE MIDI CONTROLLER - STAGE 03");
    ESP_LOGI(TAG, " BLE-MIDI PROGRAM CHANGE");
    ESP_LOGI(TAG, "======================================");

    nvs_init_safe();
    buttons_init();

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
    ESP_ERROR_CHECK(esp_ble_conn_set_pairing_allowed(true));
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
        3072,
        NULL,
        5,
        NULL);

    ESP_LOGI(TAG, "Waiting for NANOCORE BLE-MIDI...");
}
