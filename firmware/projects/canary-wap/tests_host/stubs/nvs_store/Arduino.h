/* Minimal Arduino stub for compiling the sketch's real nvs_store.h under
 * test_nvs_store_lock.cpp: Serial.printf for NvsManager::begin()'s one
 * timeout line (the stub keeps the last line and counts them), and the
 * String that Preferences::getString() returns. The FreeRTOS mutex comes
 * from firmware/tests_host/stubs/nvs_manager, the fake the canary's
 * NvsManager suite runs on. */
#ifndef STUB_NVS_STORE_ARDUINO_H
#define STUB_NVS_STORE_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

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
};
inline HostSerial Serial;

class String {
 public:
  String() = default;
  String(const char* s) : m_s(s != nullptr ? s : "") {}  // NOLINT: implicit, as Arduino's
  size_t length() const { return m_s.size(); }
  const char* c_str() const { return m_s.c_str(); }

 private:
  std::string m_s;
};

#endif
