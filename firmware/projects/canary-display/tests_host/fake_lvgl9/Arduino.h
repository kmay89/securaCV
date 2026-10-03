// tests_host/fake_lvgl9/Arduino.h — the Arduino core as ui/lvgl_port.cpp and
// canary/log.h use it: millis() for LVGL 9's tick callback and a Serial
// whose printf the test reads (the port logs a refused turn).
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

struct FakeSerial {
  std::string text;  // everything printed since the test last cleared it
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text += buf;
    return n;
  }
};
extern FakeSerial Serial;  // defined by the test

inline uint32_t millis() { return 0; }
