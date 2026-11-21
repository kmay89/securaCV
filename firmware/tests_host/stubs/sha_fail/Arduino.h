/* Minimal Arduino stub for test_sha_fail_flagship.cpp and
 * test_key_draw_check.cpp (sweeps F455, F38): the cut firmware functions
 * print through Serial and read millis(). This Serial keeps every line, so a
 * suite can hold a refusal to the line it prints. securacv_crypto.h,
 * securacv_witness.h and canary-sense's canary/log.h include <Arduino.h> for
 * these and nothing else the cut functions use. */
#ifndef STUB_SHA_FAIL_ARDUINO_H
#define STUB_SHA_FAIL_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

inline uint32_t g_host_millis = 1000;
inline uint32_t millis() { return g_host_millis; }

struct HostSerial {
  std::vector<std::string> lines;
  __attribute__((format(printf, 2, 3))) int printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::string s(buf);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    lines.push_back(s);
    return n;
  }
  void print(const char* s) { lines.push_back(s); }
  void println(uint32_t v) { lines.push_back(std::to_string(v)); }
  void println(const char* s = "") { lines.push_back(s); }
  bool said(const char* needle) const {
    for (const std::string& l : lines)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};
inline HostSerial Serial;

#endif
