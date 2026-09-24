/* Minimal Arduino stub for compiling NvsManager's own constructor,
 * destructor, begin() and end() (cut out of
 * canary/lib/securacv_crypto/src/securacv_crypto.cpp by nvs_manager_cut.awk)
 * under test_nvs_manager_lock.cpp. begin() needs Serial.printf for its one
 * timeout line; the stub keeps the last line and counts them. */
#ifndef STUB_NVS_MANAGER_ARDUINO_H
#define STUB_NVS_MANAGER_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

#endif
