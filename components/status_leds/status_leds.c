#include "status_leds.h"

#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_TASK_MS          10
#define MODE_SEQUENCE_MS   2200
#define MODE_PULSE_ON_MS     90
#define MODE_PULSE_GAP_MS   120
#define TAP_LED_ON_MS        55

static volatile controller_mode_t s_mode = CTRL_MODE_PRESET;
static volatile uint16_t s_bpm = 120;
static volatile bool s_tap_capture;
static volatile int64_t s_manual_tap_until_us;

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000ULL);

        // Green mode LED: 1/2/3 short pulses every sequence.
        uint8_t pulses = (uint8_t)s_mode + 1;
        uint32_t phase = now_ms % MODE_SEQUENCE_MS;
        bool mode_on = false;
        for (uint8_t i = 0; i < pulses; ++i) {
            uint32_t start = i * (MODE_PULSE_ON_MS + MODE_PULSE_GAP_MS);
            if (phase >= start && phase < start + MODE_PULSE_ON_MS) {
                mode_on = true;
                break;
            }
        }
        board_set_mode_led(mode_on);

        // Blue LED: quarter-note BPM pulse; manual tap gets immediate priority.
        bool tap_on = now_us < s_manual_tap_until_us;
        uint16_t bpm = s_bpm;
        if (!tap_on && bpm >= 30) {
            uint32_t beat_ms = 60000UL / bpm;
            uint32_t beat_phase = now_ms % beat_ms;
            tap_on = beat_phase < TAP_LED_ON_MS;
        }

        // While tap capture is armed, make the blue pulse a little easier
        // to identify by extending it.
        if (s_tap_capture && bpm >= 30 && !tap_on) {
            uint32_t beat_ms = 60000UL / bpm;
            tap_on = (now_ms % beat_ms) < (TAP_LED_ON_MS + 30);
        }

        board_set_tap_led(tap_on);
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_MS));
    }
}

esp_err_t status_leds_init(controller_mode_t mode, uint16_t bpm)
{
    s_mode = mode;
    s_bpm = bpm;
    if (xTaskCreate(led_task, "nc_leds", 2048, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void status_leds_set_mode(controller_mode_t mode)
{
    s_mode = mode;
}

void status_leds_set_bpm(uint16_t bpm)
{
    s_bpm = bpm;
}

void status_leds_set_tap_capture(bool active)
{
    s_tap_capture = active;
}

void status_leds_tap_feedback(void)
{
    s_manual_tap_until_us = esp_timer_get_time() + (TAP_LED_ON_MS * 1000LL);
}
