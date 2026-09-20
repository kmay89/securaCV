/* Minimal Arduino stub for host-linking the CSI HAL (csi_hal.cpp +
 * csi_features.cpp) under test_csi_hal_adapter.cpp. The HAL needs millis()
 * and, when no health_log.h bridge is on the include path, Serial.printf;
 * the test owns the fake clock. */
#ifndef STUB_CSI_HAL_ARDUINO_H
#define STUB_CSI_HAL_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

extern uint32_t g_host_millis;
static inline uint32_t millis() { return g_host_millis; }

struct HostSerial {
  int printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); const int n = vprintf(fmt, ap); va_end(ap); return n;
  }
  void println(const char* s) { puts(s); }
};
extern HostSerial Serial;

#endif
