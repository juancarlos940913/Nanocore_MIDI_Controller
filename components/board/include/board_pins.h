#pragma once

/*
 * Nanocore MIDI Controller - Hardware V2 / ESP32-S3 N16R8
 *
 * IMPORTANT:
 * This is the ONLY file that must change if the physical wiring changes.
 *
 * Pin choices intentionally avoid GPIO19/20 (USB-JTAG) and GPIO33-37
 * (reserved by Octal PSRAM on ESP32-S3R8/N16R8 modules).
 */

// ST7796S 3.5" SPI display, native 320x480, used as 480x320 landscape
#define BOARD_LCD_SCK           12
#define BOARD_LCD_MOSI          11
#define BOARD_LCD_MISO          13
#define BOARD_LCD_CS            10
#define BOARD_LCD_DC             9
#define BOARD_LCD_RST            8
#define BOARD_LCD_BL             7

// Encoders
#define BOARD_ENC1_A             4
#define BOARD_ENC1_B             5
#define BOARD_ENC1_SW            6

#define BOARD_ENC2_A            15
#define BOARD_ENC2_B            16
#define BOARD_ENC2_SW           17

#define BOARD_ENC1_REVERSE       0
#define BOARD_ENC2_REVERSE       0

// Footswitch layout
// Bottom row, left -> right
#define BOARD_FS_BOTTOM_1       18   // preset down / delete in name editor
#define BOARD_FS_BOTTOM_2       21   // preset up / cancel in name editor
#define BOARD_FS_BOTTOM_3       47   // mode short / tap-tempo long

// Top row
#define BOARD_FS_TOP_LEFT       40   // save chord / cursor left in name editor
#define BOARD_FS_TOP_RIGHT      41   // tuner chord / cursor right in name editor

// Status LEDs
#define BOARD_LED_TAP_BLUE      42
#define BOARD_LED_MODE_GREEN    39

#define BOARD_INPUT_ACTIVE_LEVEL 0
#define BOARD_LED_TAP_ACTIVE_LEVEL 1
#define BOARD_LED_MODE_ACTIVE_LEVEL 1
#define BOARD_LCD_BL_ACTIVE_LEVEL 1


// ST7796S panel tuning. Change here only if your specific module needs it.
#define BOARD_LCD_H_RES          480
#define BOARD_LCD_V_RES          320
#define BOARD_LCD_PIXEL_CLOCK_HZ 40000000
#define BOARD_LCD_INVERT_COLOR     0
#define BOARD_LCD_BGR               0
#define BOARD_LCD_MIRROR_X         1
#define BOARD_LCD_MIRROR_Y         0
#define BOARD_LCD_SWAP_XY          1
