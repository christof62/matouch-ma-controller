#pragma once

// ------------------------------------------------------------------
// MaTouch ESP32-S3 RotaryIPS-Display1.28" GC9A01 (Makerfabs MTRO128G)
// Pin mapping (verified against Makerfabs fw_test example)
// ------------------------------------------------------------------

// GC9A01 240x240 round IPS LCD (SPI)
#define GC9A01_SCLK 14
#define GC9A01_MOSI 13
#define GC9A01_MISO 12
#define GC9A01_CS   15
#define GC9A01_DC   21
#define GC9A01_RES  11
#define GC9A01_BL   7

// CST816 capacitive touch controller (I2C)
#define TOUCH_SDA 40
#define TOUCH_SCL 39
#define TOUCH_RST 16
#define TOUCH_INT 38

// Rotary encoder (EC11-style, push button)
#define ENC_CLK 9
#define ENC_DT  10
#define ENC_SW  6

// SD card (SPI, shares bus with LCD)
#define SD_SCK  14
#define SD_MISO 12
#define SD_MOSI 13
#define SD_CS   2

// ------------------------------------------------------------------
// App settings
// ------------------------------------------------------------------
#define SCREEN_W 240
#define SCREEN_H 240

#define MA_WS_PORT_DEFAULT 8095
#define POLL_INTERVAL_MS 2000
#define RECONNECT_INTERVAL_MS 5000
