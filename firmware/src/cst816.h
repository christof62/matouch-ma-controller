#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal CST816 driver (touch controller on MaTouch 1.28" GC9A01 board)
class CST816 {
public:
  bool begin(uint8_t sda, uint8_t scl, uint8_t rst, uint8_t irq);
  // Returns true when a touch was sampled; x/y in 0..239
  bool readTouch(uint8_t &x, uint8_t &y);
  bool readTouchRaw(uint8_t &x, uint8_t &y, uint8_t &gesture);

private:
  bool readRegs(uint8_t reg, uint8_t *buf, size_t len);
  uint8_t readReg(uint8_t reg);
  uint8_t _addr = 0x15;
  int _irq = -1;
  int _rst = -1;
};

// CST816 registers
#define CST816_REG_STATUS    0x00  // 0x61: idle, 0x62: press, 0x63: slide
#define CST816_REG_XPOSH     0x03
#define CST816_REG_YPOSH     0x05
#define CST816_REG_CHIP_ID   0xA7  // expect 0x14 (CST816D?) or 0xB4...
