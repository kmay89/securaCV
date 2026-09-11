/* Host stand-in for <Arduino.h> — see README.md in this directory.
 * csi_hal.cpp needs millis() and Serial.printf(); csi_features.cpp needs
 * millis(). The test defines millis() (a virtual clock). */
#ifndef SECURACV_CSI_HOST_STUB_ARDUINO_H
#define SECURACV_CSI_HOST_STUB_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <atomic>

unsigned long millis();

struct HostStubSerial {
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
  void println(const char* s) { puts(s); }
};
inline HostStubSerial Serial;

#endif /* SECURACV_CSI_HOST_STUB_ARDUINO_H */
