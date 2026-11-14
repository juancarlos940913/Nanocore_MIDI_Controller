/*
 * NANOCORE Controller - native ESP-IDF ST7789 UI
 *
 * Panel proven on the original programmer board:
 *   ST7789V 240x320
 *   SCK 12, MOSI 11, MISO 13, CS 10, DC 9, RST 8, BL 7
 *
 * Stage 04 rotates it to 320x240 landscape using esp_lcd.
 */

#include "display_ui.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_HOST                SPI2_HOST

#define PIN_LCD_SCK             12
#define PIN_LCD_MOSI            11
#define PIN_LCD_MISO            13
#define PIN_LCD_CS              10
#define PIN_LCD_DC               9
#define PIN_LCD_RST              8
#define PIN_LCD_BL               7

#define LCD_NATIVE_WIDTH       240
#define LCD_NATIVE_HEIGHT      320
#define LCD_WIDTH              320
#define LCD_HEIGHT             240
#define LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)

#define RGB565(r,g,b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BLACK     RGB565(0,0,0)
#define C_BG        RGB565(10,13,18)
#define C_PANEL     RGB565(24,29,38)
#define C_PANEL2    RGB565(34,41,52)
#define C_WHITE     RGB565(245,247,250)
#define C_MUTED     RGB565(135,145,160)
#define C_GREEN     RGB565(50,220,120)
#define C_RED       RGB565(235,76,92)
#define C_CYAN      RGB565(62,201,220)
#define C_BLUE      RGB565(60,112,240)
#define C_ORANGE    RGB565(245,166,65)
#define C_YELLOW    RGB565(245,210,70)

static const char *TAG = "NC_DISPLAY";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static SemaphoreHandle_t s_ui_mutex = NULL;
static bool s_ready = false;

typedef struct {
    char c;
    uint8_t row[7]; // five LSBs used
} glyph_t;

static const glyph_t FONT[] = {
    {' ',{0,0,0,0,0,0,0}},
    {'-',{0,0,0,31,0,0,0}},
    {'+',{0,4,4,31,4,4,0}},
    {':',{0,4,4,0,4,4,0}},
    {'/',{1,2,4,8,16,0,0}},
    {'?',{14,17,1,2,4,0,4}},
    {'0',{14,17,19,21,25,17,14}},
    {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}},
    {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}},
    {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}},
    {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,1,14}},
    {'A',{14,17,17,31,17,17,17}},
    {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}},
    {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}},
    {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}},
    {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}},
    {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}},
    {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}},
    {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}},
    {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}},
    {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}},
    {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},
    {'Z',{31,1,2,4,8,16,31}},
};

static bool color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                          esp_lcd_panel_io_event_data_t *edata,
                          void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

static const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }

    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); ++i) {
        if (FONT[i].c == c) {
            return FONT[i].row;
        }
    }

    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); ++i) {
        if (FONT[i].c == '?') {
            return FONT[i].row;
        }
    }
    return NULL;
}

static inline void put_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= LCD_WIDTH || (unsigned)y >= LCD_HEIGHT || !s_fb) {
        return;
    }
    s_fb[y * LCD_WIDTH + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = y; yy < y + h; ++yy) {
        uint16_t *row = &s_fb[yy * LCD_WIDTH + x];
        for (int xx = 0; xx < w; ++xx) row[xx] = color;
    }
}

static void draw_rect(int x, int y, int w, int h, uint16_t color)
{
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static void draw_char(int x, int y, char c, uint16_t color, int scale)
{
    const uint8_t *rows = glyph_rows(c);
    if (!rows || scale < 1) return;

    for (int ry = 0; ry < 7; ++ry) {
        for (int rx = 0; rx < 5; ++rx) {
            if (rows[ry] & (1u << (4 - rx))) {
                fill_rect(x + rx * scale,
                          y + ry * scale,
                          scale,
                          scale,
                          color);
            }
        }
    }
}

static int text_width(const char *s, int scale)
{
    if (!s || !*s) return 0;
    return (int)strlen(s) * (6 * scale) - scale;
}

static void draw_text(int x, int y, const char *s, uint16_t color, int scale)
{
    if (!s) return;
    for (; *s; ++s) {
        draw_char(x, y, *s, color, scale);
        x += 6 * scale;
    }
}

static void draw_text_center(int cx, int y, const char *s, uint16_t color, int scale)
{
    draw_text(cx - text_width(s, scale) / 2, y, s, color, scale);
}

static const char *mode_name(ui_mode_t mode)
{
    switch (mode) {
        case UI_MODE_STOMP: return "STOMP";
        case UI_MODE_SCENE: return "SCENE";
        case UI_MODE_PRESET:
        default: return "PRESET";
    }
}

static uint16_t block_color(ui_block_state_t state)
{
    switch (state) {
        case UI_BLOCK_ON: return C_GREEN;
        case UI_BLOCK_OFF: return C_RED;
        case UI_BLOCK_PENDING_ON:
        case UI_BLOCK_PENDING_OFF: return C_YELLOW;
        case UI_BLOCK_UNKNOWN:
        default: return C_MUTED;
    }
}

static const char *block_suffix(ui_block_state_t state)
{
    switch (state) {
        case UI_BLOCK_ON: return "ON";
        case UI_BLOCK_OFF: return "OFF";
        case UI_BLOCK_PENDING_ON: return "ON?";
        case UI_BLOCK_PENDING_OFF: return "OFF?";
        case UI_BLOCK_UNKNOWN:
        default: return "?";
    }
}

static void draw_block(int x, int y, int w, int h,
                       const char *label, ui_block_state_t state)
{
    uint16_t color = block_color(state);
    fill_rect(x, y, w, h, C_PANEL);
    draw_rect(x, y, w, h, color);

    draw_text_center(x + w / 2, y + 8, label, C_WHITE, 1);
    draw_text_center(x + w / 2, y + 23, block_suffix(state), color, 1);
}

static void draw_softkeys(ui_mode_t mode)
{
    fill_rect(0, 206, LCD_WIDTH, 34, C_PANEL2);

    const char *left = "PREV";
    const char *right = "NEXT";

    if (mode == UI_MODE_STOMP) {
        left = "FX2";
        right = "DELAY";
    } else if (mode == UI_MODE_SCENE) {
        left = "SCN -";
        right = "SCN +";
    }

    draw_text(10, 214, left, C_WHITE, 1);
    draw_text(LCD_WIDTH - 10 - text_width(right, 1), 214, right, C_WHITE, 1);
    draw_text_center(LCD_WIDTH / 2, 226, "HOLD BOTH: MODE", C_MUTED, 1);
}

static void draw_header(const display_ui_state_t *s)
{
    fill_rect(0, 0, LCD_WIDTH, 31, C_PANEL2);

    draw_text(9, 8, "NANOCORE CTRL", C_WHITE, 1);

    uint16_t link_color = s->midi_ready ? C_GREEN :
                          s->ble_connected ? C_YELLOW : C_RED;

    fill_rect(207, 8, 8, 8, link_color);
    draw_text(221, 8,
              s->midi_ready ? "MIDI" :
              (s->ble_connected ? "BLE" : "SCAN"),
              link_color, 1);

    const char *mode = mode_name(s->mode);
    int mw = text_width(mode, 1);
    draw_text(LCD_WIDTH - mw - 8, 19, mode, C_CYAN, 1);
}

static void draw_main_value(const display_ui_state_t *s)
{
    char buf[32];

    if (s->mode == UI_MODE_SCENE) {
        char scene_letter = (char)('A' + (s->scene & 0x03));
        snprintf(buf, sizeof(buf), "SCENE %c", scene_letter);
        draw_text_center(LCD_WIDTH / 2, 40, buf, C_ORANGE, 2);
    } else {
        snprintf(buf, sizeof(buf), "PC %02u", s->preset);
        draw_text_center(LCD_WIDTH / 2, 40, buf,
                         s->mode == UI_MODE_STOMP ? C_GREEN : C_CYAN, 2);
    }
}

static esp_err_t flush_framebuffer(void)
{
    while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) {
        // drain stale completion token
    }

    esp_err_t rc = esp_lcd_panel_draw_bitmap(
        s_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_fb);

    if (rc != ESP_OK) {
        return rc;
    }

    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD flush timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t display_ui_init(void)
{
    if (s_ready) return ESP_OK;

    s_ui_mutex = xSemaphoreCreateMutex();
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_ui_mutex || !s_flush_done) {
        return ESP_ERR_NO_MEM;
    }

    s_fb = heap_caps_malloc(
        LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!s_fb) {
        ESP_LOGE(TAG, "Could not allocate %u-byte DMA framebuffer",
                 (unsigned)(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_LCD_BL, 0);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };

    esp_err_t rc = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(rc));
        return rc;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = color_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // Equivalent orientation to Adafruit_ST7789 rotation 1:
    // 240x320 native -> 320x240 landscape.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));

    // This panel is RGB. With native esp_lcd the previous Arduino
    // invertDisplay(true) setting produces inverted colors, so keep
    // the panel in normal color mode here.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    ESP_ERROR_CHECK(flush_framebuffer());

    gpio_set_level(PIN_LCD_BL, 1);

    s_ready = true;
    ESP_LOGI(TAG, "ST7789 ready: 320x240 landscape");
    return ESP_OK;
}

bool display_ui_is_ready(void)
{
    return s_ready;
}

void display_ui_render(const display_ui_state_t *s)
{
    if (!s_ready || !s || !s_fb || !s_panel) return;

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);

    draw_header(s);
    draw_main_value(s);

    // Two rows x four blocks: signal chain view.
    const int x0 = 8;
    const int gap = 5;
    const int bw = 72;
    const int bh = 48;
    const int y1 = 82;
    const int y2 = 135;

    draw_block(x0 + 0 * (bw + gap), y1, bw, bh, "FX1", s->fx1);
    draw_block(x0 + 1 * (bw + gap), y1, bw, bh, "FX2", s->fx2);
    draw_block(x0 + 2 * (bw + gap), y1, bw, bh, "AMP", s->amp);
    draw_block(x0 + 3 * (bw + gap), y1, bw, bh, "CAB", s->cab);

    draw_block(x0 + 0 * (bw + gap), y2, bw, bh, "MOD", s->mod);
    draw_block(x0 + 1 * (bw + gap), y2, bw, bh, "DLY", s->delay);
    draw_block(x0 + 2 * (bw + gap), y2, bw, bh, "REV", s->reverb);
    draw_block(x0 + 3 * (bw + gap), y2, bw, bh, "EQ", s->eq);

    draw_softkeys(s->mode);

    esp_err_t rc = flush_framebuffer();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "UI render failed: %s", esp_err_to_name(rc));
    }

    xSemaphoreGive(s_ui_mutex);
}
