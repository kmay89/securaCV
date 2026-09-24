/* Minimal Arduino stub for compiling securacv_witness.cpp's chain-persist
 * glue (cut out by cut_functions.awk) under test_chain_persist.cpp: a Serial
 * that keeps its last line and counts them, and a millis() the test sets.
 * securacv_witness.h includes <Arduino.h> for these and nothing else the cut
 * functions use. */
#ifndef STUB_WITNESS_GLUE_ARDUINO_H
#define STUB_WITNESS_GLUE_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

inline uint32_t g_host_millis = 0;
inline uint32_t millis() { return g_host_millis; }

struct HostSerial {
  int lines = 0;
  char last[192] = {0};
  int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(last, sizeof(last), fmt, ap);
    va_end(ap);
    lines++;
    return n;
  }
  void print(const char* s) { snprintf(last, sizeof(last), "%s", s); }
  void println(uint32_t v) { snprintf(last, sizeof(last), "%u", (unsigned)v); lines++; }
  void println(const char* s = "") { snprintf(last, sizeof(last), "%s", s); lines++; }
};
inline HostSerial Serial;

#endif
