#include "input_manager.h"

#include <stdbool.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define POLL_MS            10
#define DEBOUNCE_MS        30
#define MODE_LONG_MS      650
#define CHORD_HOLD_MS      70

static const char *TAG = "NC_INPUT";
static QueueHandle_t s_queue;

typedef struct {
    gpio_num_t gpio;
    bool raw;
    bool stable;
    bool consumed;
    bool long_fired;
    TickType_t raw_change;
    TickType_t press_tick;
} button_t;

typedef struct {
    gpio_num_t a;
    gpio_num_t b;
    gpio_num_t sw;
    int last_a;
    bool sw_raw;
    bool sw_stable;
    TickType_t sw_change;
} encoder_t;

static void emit(input_event_t e)
{
    if (e == INPUT_NONE || !s_queue) return;
    (void)xQueueSend(s_queue, &e, 0);
}

static bool is_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == BOARD_INPUT_ACTIVE_LEVEL;
}

static bool button_poll(button_t *b, TickType_t now)
{
    bool raw = is_pressed(b->gpio);
    if (raw != b->raw) {
        b->raw = raw;
        b->raw_change = now;
    }

    if ((now - b->raw_change) < pdMS_TO_TICKS(DEBOUNCE_MS)) {
        return false;
    }

    if (raw == b->stable) return false;

    b->stable = raw;
    if (raw) {
        b->press_tick = now;
        b->consumed = false;
        b->long_fired = false;
    }
    return true;
}

static int encoder_step(encoder_t *e)
{
    int a = gpio_get_level(e->a);
    int step = 0;

    // Same edge/direction rule as the user's proven programmer firmware.
    if (a != e->last_a && a == 0) {
        int b = gpio_get_level(e->b);
        step = (b != a) ? 1 : -1;
    }

    e->last_a = a;
    return step;
}

static bool encoder_click(encoder_t *e, TickType_t now)
{
    bool raw = is_pressed(e->sw);

    if (raw != e->sw_raw) {
        e->sw_raw = raw;
        e->sw_change = now;
    }

    if ((now - e->sw_change) < pdMS_TO_TICKS(DEBOUNCE_MS)) {
        return false;
    }

    if (raw == e->sw_stable) return false;

    e->sw_stable = raw;
    return raw; // click on debounced press edge
}

static input_event_t short_event_for(gpio_num_t gpio)
{
    if (gpio == BOARD_FS_BOTTOM_1) return INPUT_FS_PRESET_DOWN;
    if (gpio == BOARD_FS_BOTTOM_2) return INPUT_FS_PRESET_UP;
    if (gpio == BOARD_FS_BOTTOM_3) return INPUT_FS_MODE_SHORT;
    if (gpio == BOARD_FS_TOP_LEFT) return INPUT_FS_TOP_LEFT;
    if (gpio == BOARD_FS_TOP_RIGHT) return INPUT_FS_TOP_RIGHT;
    return INPUT_NONE;
}

static void maybe_release_short(button_t *b, bool changed)
{
    if (!changed || b->stable) return;

    if (!b->consumed && !b->long_fired) {
        emit(short_event_for(b->gpio));
    }
}

static void input_task(void *arg)
{
    (void)arg;
    TickType_t now = xTaskGetTickCount();

    button_t down = {.gpio = BOARD_FS_BOTTOM_1, .raw_change = now};
    button_t up   = {.gpio = BOARD_FS_BOTTOM_2, .raw_change = now};
    button_t mode = {.gpio = BOARD_FS_BOTTOM_3, .raw_change = now};
    button_t tl   = {.gpio = BOARD_FS_TOP_LEFT, .raw_change = now};
    button_t tr   = {.gpio = BOARD_FS_TOP_RIGHT, .raw_change = now};

    encoder_t e1 = {
        .a = BOARD_ENC1_A, .b = BOARD_ENC1_B, .sw = BOARD_ENC1_SW,
        .last_a = gpio_get_level(BOARD_ENC1_A), .sw_change = now,
    };
    encoder_t e2 = {
        .a = BOARD_ENC2_A, .b = BOARD_ENC2_B, .sw = BOARD_ENC2_SW,
        .last_a = gpio_get_level(BOARD_ENC2_A), .sw_change = now,
    };

    bool save_chord_active = false;
    bool tuner_chord_active = false;
    TickType_t save_chord_start = 0;
    TickType_t tuner_chord_start = 0;

    while (1) {
        now = xTaskGetTickCount();

        bool ch_down = button_poll(&down, now);
        bool ch_up   = button_poll(&up, now);
        bool ch_mode = button_poll(&mode, now);
        bool ch_tl   = button_poll(&tl, now);
        bool ch_tr   = button_poll(&tr, now);

        int s1 = encoder_step(&e1);
        int s2 = encoder_step(&e2);
        if (BOARD_ENC1_REVERSE) s1 = -s1;
        if (BOARD_ENC2_REVERSE) s2 = -s2;

        if (s1 < 0) emit(INPUT_ENC1_LEFT);
        if (s1 > 0) emit(INPUT_ENC1_RIGHT);
        if (s2 < 0) emit(INPUT_ENC2_LEFT);
        if (s2 > 0) emit(INPUT_ENC2_RIGHT);

        if (encoder_click(&e1, now)) emit(INPUT_ENC1_CLICK);
        if (encoder_click(&e2, now)) emit(INPUT_ENC2_CLICK);

        // SAVE chord: bottom-left + top-left.
        if (down.stable && tl.stable) {
            if (!save_chord_active) {
                save_chord_active = true;
                save_chord_start = now;
            }
            if ((now - save_chord_start) >= pdMS_TO_TICKS(CHORD_HOLD_MS) &&
                !down.consumed && !tl.consumed) {
                down.consumed = true;
                tl.consumed = true;
                emit(INPUT_CHORD_SAVE);
            }
        } else if (!down.stable && !tl.stable) {
            save_chord_active = false;
        }

        // TUNER chord: mode/tap + top-right.
        if (mode.stable && tr.stable) {
            if (!tuner_chord_active) {
                tuner_chord_active = true;
                tuner_chord_start = now;
            }
            if ((now - tuner_chord_start) >= pdMS_TO_TICKS(CHORD_HOLD_MS) &&
                !mode.consumed && !tr.consumed) {
                mode.consumed = true;
                tr.consumed = true;
                emit(INPUT_CHORD_TUNER);
            }
        } else if (!mode.stable && !tr.stable) {
            tuner_chord_active = false;
        }

        // Long bottom-center enters/exits tap-capture context.
        if (mode.stable && !mode.consumed && !mode.long_fired &&
            (now - mode.press_tick) >= pdMS_TO_TICKS(MODE_LONG_MS)) {
            mode.long_fired = true;
            mode.consumed = true;
            emit(INPUT_FS_MODE_LONG);
        }

        maybe_release_short(&down, ch_down);
        maybe_release_short(&up, ch_up);
        maybe_release_short(&mode, ch_mode);
        maybe_release_short(&tl, ch_tl);
        maybe_release_short(&tr, ch_tr);

        TickType_t delay_ticks = pdMS_TO_TICKS(POLL_MS);
        if (delay_ticks < 1) {
            delay_ticks = 1;
        }
        vTaskDelay(delay_ticks);
    }
}

esp_err_t input_manager_init(void)
{
    s_queue = xQueueCreate(32, sizeof(input_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(input_task, "nc_input", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Input manager ready");
    return ESP_OK;
}

bool input_manager_get_event(input_event_t *event, TickType_t timeout)
{
    if (!event || !s_queue) return false;
    return xQueueReceive(s_queue, event, timeout) == pdTRUE;
}
