#pragma once
#include <Arduino.h>

namespace canary {

static inline uint32_t ms_now() { return millis(); }

// Central place to choose the debug port
static inline HardwareSerial& dbg_serial() {
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

// ---- Backwards-compatible global wrappers ----
// (So existing code can keep calling log_line(), ms_now(), etc.)
static inline uint32_t ms_now() { return canary::ms_now(); }
static inline void log_line(const char* tag, const char* msg) { canary::log_line(tag, msg); }
static inline void log_kv(const char* tag, const char* k, const char* v) { canary::log_kv(tag, k, v); }
