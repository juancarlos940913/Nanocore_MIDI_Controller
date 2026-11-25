#include "input_manager.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
    volatile int16_t quarter_accum;
    volatile int16_t detent_accum;
    volatile uint8_t last_state;
    bool sw_raw;
    bool sw_stable;
    TickType_t sw_change;
} encoder_t;

static encoder_t s_enc1 = {
    .a = BOARD_ENC1_A,
    .b = BOARD_ENC1_B,
    .sw = BOARD_ENC1_SW,
};

static encoder_t s_enc2 = {
    .a = BOARD_ENC2_A,
    .b = BOARD_ENC2_B,
    .sw = BOARD_ENC2_SW,
};

static portMUX_TYPE s_encoder_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Gray-code transition table. Invalid/bounce transitions contribute zero.
 * This follows every A/B edge instead of periodically sampling one edge,
 * so fast physical turns are not misread as backwards movement.
 */
static const int8_t QUAD_TABLE[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

static void emit(input_event_t e)
{
    if (e == INPUT_NONE || !s_queue) return;
    (void)xQueueSend(s_queue, &e, 0);
}

static bool is_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == BOARD_INPUT_ACTIVE_LEVEL;
}

static void IRAM_ATTR encoder_isr(void *arg)
{
    encoder_t *e = (encoder_t *)arg;
    const uint8_t state =
        (uint8_t)((gpio_get_level(e->a) ? 2 : 0) |
                  (gpio_get_level(e->b) ? 1 : 0));

    const uint8_t index = (uint8_t)((e->last_state << 2) | state);
    const int8_t step = QUAD_TABLE[index & 0x0F];
    e->last_state = state;

    if (step == 0) return;

    portENTER_CRITICAL_ISR(&s_encoder_mux);
    e->quarter_accum += step;

    if (e->quarter_accum >= BOARD_ENCODER_EDGES_PER_DETENT) {
        e->detent_accum++;
        e->quarter_accum = 0;
    } else if (e->quarter_accum <= -BOARD_ENCODER_EDGES_PER_DETENT) {
        e->detent_accum--;
        e->quarter_accum = 0;
    }
    portEXIT_CRITICAL_ISR(&s_encoder_mux);
}

static bool button_poll(button_t *b, TickType_t now)
{
    bool raw = is_pressed(b->gpio);
    if (raw != b->raw) {
        b->raw = raw;
        b->raw_change = now;
    }

    if ((now - b->raw_change) < pdMS_TO_TICKS(DEBOUNCE_MS)) return false;
    if (raw == b->stable) return false;

    b->stable = raw;
    if (raw) {
        b->press_tick = now;
        b->consumed = false;
        b->long_fired = false;
    }
    return true;
}

static bool encoder_click(encoder_t *e, TickType_t now)
{
    bool raw = is_pressed(e->sw);

    if (raw != e->sw_raw) {
        e->sw_raw = raw;
        e->sw_change = now;
    }

    if ((now - e->sw_change) < pdMS_TO_TICKS(DEBOUNCE_MS)) return false;
    if (raw == e->sw_stable) return false;

    e->sw_stable = raw;
    return raw;
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
    if (!b->consumed && !b->long_fired) emit(short_event_for(b->gpio));
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

    s_enc1.sw_change = now;
    s_enc2.sw_change = now;

    bool save_chord_active = false;
    bool tuner_chord_active = false;
    bool mapper_chord_active = false;
    TickType_t save_chord_start = 0;
    TickType_t tuner_chord_start = 0;
    TickType_t mapper_chord_start = 0;

    while (1) {
        now = xTaskGetTickCount();

        bool ch_down = button_poll(&down, now);
        bool ch_up   = button_poll(&up, now);
        bool ch_mode = button_poll(&mode, now);
        bool ch_tl   = button_poll(&tl, now);
        bool ch_tr   = button_poll(&tr, now);

        if (encoder_click(&s_enc1, now)) emit(INPUT_ENC1_CLICK);
        if (encoder_click(&s_enc2, now)) emit(INPUT_ENC2_CLICK);

        // SAVE stays on physical bottom #1 + top-left. After the wiring
        // correction, physical bottom #1 is the 'up' button object.
        if (up.stable && tl.stable) {
            if (!save_chord_active) {
                save_chord_active = true;
                save_chord_start = now;
            }
            if ((now - save_chord_start) >= pdMS_TO_TICKS(CHORD_HOLD_MS) &&
                !up.consumed && !tl.consumed) {
                up.consumed = true;
                tl.consumed = true;
                emit(INPUT_CHORD_SAVE);
            }
        } else if (!up.stable && !tl.stable) {
            save_chord_active = false;
        }

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

        // Diagnostic Live Mapper: hold both top footswitches together.
        // Both individual actions are consumed so no Scene/STOMP action leaks.
        if (tl.stable && tr.stable) {
            if (!mapper_chord_active) {
                mapper_chord_active = true;
                mapper_chord_start = now;
            }
            if ((now - mapper_chord_start) >= pdMS_TO_TICKS(CHORD_HOLD_MS) &&
                !tl.consumed && !tr.consumed) {
                tl.consumed = true;
                tr.consumed = true;
                emit(INPUT_CHORD_MAPPER);
            }
        } else if (!tl.stable && !tr.stable) {
            mapper_chord_active = false;
        }

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

        vTaskDelay(pdMS_TO_TICKS(POLL_MS) > 0 ? pdMS_TO_TICKS(POLL_MS) : 1);
    }
}

static esp_err_t init_encoder_isr(encoder_t *e)
{
    e->last_state =
        (uint8_t)((gpio_get_level(e->a) ? 2 : 0) |
                  (gpio_get_level(e->b) ? 1 : 0));

    ESP_ERROR_CHECK(gpio_set_intr_type(e->a, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(e->b, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(e->a, encoder_isr, e));
    ESP_ERROR_CHECK(gpio_isr_handler_add(e->b, encoder_isr, e));
    return ESP_OK;
}

esp_err_t input_manager_init(void)
{
    s_queue = xQueueCreate(24, sizeof(input_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    esp_err_t rc = gpio_install_isr_service(0);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) return rc;

    ESP_ERROR_CHECK(init_encoder_isr(&s_enc1));
    ESP_ERROR_CHECK(init_encoder_isr(&s_enc2));

    if (xTaskCreate(input_task, "nc_input", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Input manager ready (interrupt quadrature + accumulated detents)");
    return ESP_OK;
}

bool input_manager_get_event(input_event_t *event, TickType_t timeout)
{
    if (!event || !s_queue) return false;
    return xQueueReceive(s_queue, event, timeout) == pdTRUE;
}

int input_manager_take_encoder_delta(uint8_t encoder_index)
{
    encoder_t *e = encoder_index == 1 ? &s_enc1 :
                   encoder_index == 2 ? &s_enc2 : NULL;
    if (!e) return 0;

    portENTER_CRITICAL(&s_encoder_mux);
    int delta = e->detent_accum;
    e->detent_accum = 0;
    portEXIT_CRITICAL(&s_encoder_mux);

    if ((encoder_index == 1 && BOARD_ENC1_REVERSE) ||
        (encoder_index == 2 && BOARD_ENC2_REVERSE)) {
        delta = -delta;
    }
    return delta;
}
