#pragma once

// ============================================================
// Existing controller PCB - ESP32-S3 N16R8
// ============================================================

// SPI / TFT / SD
#define SPI_SCK         12
#define SPI_MOSI        11
#define SPI_MISO        13

#define TFT_CS          10
#define TFT_DC           9
#define TFT_RST          8
#define TFT_BL           7

#define SD_CS           14

// Encoders
#define ENC1_A           4
#define ENC1_B           5
#define ENC1_SW          6

#define ENC2_A          15
#define ENC2_B          16
#define ENC2_SW         17

// Buttons
#define BTN_PROGRAM     18
#define BTN_STOP        21
#define BTN_FUNCTION    47

// INA219
#define INA_SDA          1
#define INA_SCL          2

// AVR ISP - not used by Nanocore MIDI Controller
#define AVR_MOSI        38
#define AVR_MISO        39
#define AVR_SCK         40
#define AVR_RESET       41
