#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace canary {

static inline uint32_t ms_now() { return millis(); }

// Pick a serial port that exists across ESP32 variants / CI compiles.
// - Most Arduino-ESP32 builds provide Serial.
// - If not, fall back to Serial1.
static inline HardwareSerial& dbg_serial() {
#ifdef Serial
  return Serial;
#else
  return Serial1;
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
