#include "cst816.h"

bool CST816::begin(uint8_t sda, uint8_t scl, uint8_t rst, uint8_t irq) {
  _rst = rst;
  _irq = irq;

  Wire.begin(sda, scl);
  Wire.setClock(400000);
  delay(50);

  pinMode(_rst, OUTPUT);
  digitalWrite(_rst, LOW);
  delay(20);
  digitalWrite(_rst, HIGH);
  delay(120);

  pinMode(_irq, INPUT);

  // scan for the touch controller (CST816 variants use 0x14 or 0x15)
  uint8_t found = 0;
  for (uint8_t a = 0x10; a <= 0x20; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[TOUCH] I2C device found at 0x%02X\n", a);
      if (found == 0) found = a;
    }
  }
  if (found == 0) {
    Serial.println("[TOUCH] WARNING: no I2C device found - touch disabled");
    _addr = 0;  // mark as absent
  } else {
    _addr = found;
  }
  uint8_t id = readReg(CST816_REG_CHIP_ID);
  Serial.printf("[TOUCH] chip id: 0x%02X\n", id);

  return found != 0;
}

uint8_t CST816::readReg(uint8_t reg) {
  uint8_t v = 0;
  readRegs(reg, &v, 1);
  return v;
}

bool CST816::readRegs(uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t n = Wire.requestFrom(_addr, len);
  if (n != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool CST816::readTouch(uint8_t &x, uint8_t &y) {
  uint8_t g;
  return readTouchRaw(x, y, g);
}

bool CST816::readTouchRaw(uint8_t &x, uint8_t &y, uint8_t &gesture) {
  // touch controller absent -> never report touches
  if (_addr == 0) return false;

  // CST816 IRQ line is LOW while touched: skip bus access entirely otherwise
  if (_irq >= 0 && digitalRead(_irq) == HIGH) {
    return false;
  }

  uint8_t buf[6];
  if (!readRegs(CST816_REG_STATUS, buf, 6)) return false;

  // no touch: status nibble 1 = idle
  if ((buf[0] & 0x0F) == 0x01 || (buf[0] & 0x0F) == 0x00) {
    return false;
  }

  x = ((buf[2] & 0x0F) << 8) | buf[3];
  y = ((buf[4] & 0x0F) << 8) | buf[5];
  gesture = buf[1];
  return true;
}
