// canary-local/emulator/shim/Wire.h — the I2C silicon boundary, browser-side.
//
// The touch169 flavor compiles the battery-backed RTC transport
// (src/io/rtc.cpp, FEATURE_RTC) — real I2C traffic to a PCF85063A on the
// glass, meaningless in a browser. This TwoWire answers like an empty bus:
// every transmission NACKs and every read returns nothing, so the RTC
// probe concludes "not populated" and the firmware takes the same
// no-RTC path it takes on hardware with the pad unstuffed. A quiet probe
// is normal, not fatal — exactly the real driver's contract.
#pragma once

#include <stdint.h>
#include <stddef.h>

class TwoWire {
 public:
  void begin(int sda = -1, int scl = -1, uint32_t freq = 0) {
    (void)sda; (void)scl; (void)freq;
  }
  void beginTransmission(uint8_t addr) { (void)addr; }
  size_t write(uint8_t b) { (void)b; return 1; }
  size_t write(const uint8_t* buf, size_t len) { (void)buf; return len; }
  // 2 = NACK on address: nothing lives on the emulated bus.
  uint8_t endTransmission(bool stop = true) { (void)stop; return 2; }
  uint8_t requestFrom(int addr, int len) { (void)addr; (void)len; return 0; }
  int available() { return 0; }
  int read() { return -1; }
};

inline TwoWire Wire;
