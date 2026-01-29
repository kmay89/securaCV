#pragma once
#include <Arduino.h>

namespace canary {

static inline uint32_t ms_now() { return millis(); }

// Central place to choose the debug port
static inline HardwareSerial& dbg_serial() {
  // ESP32-C3 often prefers Serial, but some configs only expose Serial0/Serial1.
  // Serial0 is the USB CDC/UART on many ESP32 cores.
  #if defined(ARDUINO_ARCH_ESP32)
    #if defined(Serial0)
      return Serial0;
    #else
      return Serial;
    #endif
  #else
    return Serial;
  #endif
}

static inline void log_header(const char* tag) {
  dbg_serial().printf("[%08lu][%s] ", (unsigned long)ms_now(), tag);
}

static inline void log_line(const char* tag, const char* msg) {
  log_header(tag);
  dbg_serial().println(msg);
}

static inline void log_kv(const char* tag, const char* k, const char* v) {
  log_header(tag);
  dbg_serial().printf("%s=%s\n", k, v);
}

} // namespace canary
