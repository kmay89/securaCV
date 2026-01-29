// include/canary/log.h
#pragma once
#include <Arduino.h>

namespace canary {

static inline HardwareSerial& dbg_serial() {
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
  return Serial;
#else
  // Fallback: still compiles in weird CI targets
  return Serial1;
#endif
}

static inline uint32_t ms_now() { return millis(); }

static inline void log_header(const char* tag) {
  dbg_serial().printf("\n[%s]\n", tag);
}

static inline void log_line(const char* tag, const char* msg) {
  dbg_serial().printf("[%s] %s\n", tag, msg);
}

} // namespace canary

