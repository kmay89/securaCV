// canary-local/emulator/test/native9/Arduino.h — the Arduino core as
// ui/lvgl_port.cpp and canary/log.h use it, for glass_turn_lvgl9.sh (F225):
// millis() for LVGL 9's tick callback (glass_turn_lvgl9_test.cpp defines
// it) and a Serial whose printf the test reads (the port logs a refused
// turn). Nothing else of the core is reached from the port.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Native9Serial {
  char text[4096] = {0};  // everything printed since the test last cleared it
  size_t len = 0;
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(text + len, sizeof(text) - len, fmt, ap);
    va_end(ap);
    if (n > 0) len = (len + (size_t)n < sizeof(text)) ? len + (size_t)n : sizeof(text) - 1;
    return n;
  }
  void clear() {
    len = 0;
    text[0] = 0;
  }
  bool saw(const char* needle) const { return strstr(text, needle) != nullptr; }
};
extern Native9Serial Serial;  // defined by the test

extern "C" uint32_t millis(void);  // defined by the test
