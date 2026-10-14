// tests_host/fake_fieldbus/Wire.h — an I2C bus with nothing on it, for the
// playground's census and sensor polls in test_fieldbus_glue.cpp: every
// address NACKs (endTransmission() returns 2, as TwoWire does for an address
// NACK) and every read comes back empty.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct FakeWire {
  void beginTransmission(int addr) { (void)addr; }
  size_t write(uint8_t b) {
    (void)b;
    return 1;
  }
  uint8_t endTransmission(bool stop = true) {
    (void)stop;
    return 2;
  }
  uint8_t requestFrom(int addr, int len) {
    (void)addr;
    (void)len;
    return 0;
  }
  int read() { return -1; }
};
inline FakeWire Wire;
