/* Minimal Arduino stub for test_nvs_load_key.cpp: the flagship's key load and
 * store (securacv_crypto.cpp) and establish_identity_key()
 * (securacv_witness.cpp), cut out by cut_functions.awk, print through Serial
 * and read millis(). This Serial keeps every line, so the suite can hold a
 * refusal to the two lines it prints. securacv_witness.h and
 * securacv_crypto.h include <Arduino.h> for these and nothing else the cut
 * functions use. */
#ifndef STUB_NVS_KEY_ARDUINO_H
#define STUB_NVS_KEY_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

inline uint32_t g_host_millis = 0;
inline uint32_t millis() { return g_host_millis; }

struct HostSerial {
  std::vector<std::string> lines;
  int printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::string s(buf);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    lines.push_back(s);
    return n;
  }
  void print(const char* s) { lines.push_back(s); }
  void println(uint32_t v) { lines.push_back(std::to_string(v)); }
  void println(const char* s = "") { lines.push_back(s); }
};
inline HostSerial Serial;

#endif
