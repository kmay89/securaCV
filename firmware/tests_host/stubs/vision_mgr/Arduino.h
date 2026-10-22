/* Arduino.h for test_vision_module_link.cpp: canary-vision's REAL
 * src/vision/vision_mgr.cpp on the host. Serial keeps everything printed in
 * a string the suite reads back (the boot lines are a contract: gen_vision.py
 * stages them and the desktop Flasher's monitor parses them), and delay()
 * only records how long the firmware asked to wait. */
#ifndef STUB_VISION_MGR_ARDUINO_H
#define STUB_VISION_MGR_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

struct StubSerial {
  std::string out;
  void print(const char* s) { out += s; }
  void println(const char* s) { out += s; out += "\n"; }
  void println() { out += "\n"; }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out += buf;
    return n;
  }
};
inline StubSerial Serial;

inline uint32_t& stub_delay_total_ms() {
  static uint32_t total = 0;
  return total;
}
inline void delay(uint32_t ms) { stub_delay_total_ms() += ms; }
inline uint32_t millis() { return 1000; }

#endif
