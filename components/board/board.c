#include "board.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "NC_BOARD";

esp_err_t board_init(void)
{
    const uint64_t input_mask =
        (1ULL << BOARD_ENC1_A) |
        (1ULL << BOARD_ENC1_B) |
        (1ULL << BOARD_ENC1_SW) |
        (1ULL << BOARD_ENC2_A) |
        (1ULL << BOARD_ENC2_B) |
        (1ULL << BOARD_ENC2_SW) |
        (1ULL << BOARD_FS_BOTTOM_1) |
        (1ULL << BOARD_FS_BOTTOM_2) |
        (1ULL << BOARD_FS_BOTTOM_3) |
        (1ULL << BOARD_FS_TOP_LEFT) |
        (1ULL << BOARD_FS_TOP_RIGHT);

    gpio_config_t in_cfg = {
        .pin_bit_mask = input_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t rc = gpio_config(&in_cfg);
    if (rc != ESP_OK) return rc;

    const uint64_t output_mask =
        (1ULL << BOARD_LED_TAP_BLUE) |
        (1ULL << BOARD_LED_MODE_GREEN) |
        (1ULL << BOARD_LCD_BL);

    gpio_config_t out_cfg = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    rc = gpio_config(&out_cfg);
    if (rc != ESP_OK) return rc;

    board_set_tap_led(false);
    board_set_mode_led(false);
    board_set_backlight(false);

    ESP_LOGI(TAG, "Hardware V2 GPIO initialized");
    return ESP_OK;
}

void board_set_tap_led(bool on)
{
    gpio_set_level(BOARD_LED_TAP_BLUE,
                   on ? BOARD_LED_TAP_ACTIVE_LEVEL
                      : !BOARD_LED_TAP_ACTIVE_LEVEL);
}

void board_set_mode_led(bool on)
{
    gpio_set_level(BOARD_LED_MODE_GREEN,
                   on ? BOARD_LED_MODE_ACTIVE_LEVEL
                      : !BOARD_LED_MODE_ACTIVE_LEVEL);
}

void board_set_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_BL,
                   on ? BOARD_LCD_BL_ACTIVE_LEVEL
                      : !BOARD_LCD_BL_ACTIVE_LEVEL);
}
