#pragma once
#include <Arduino.h>

namespace canary {

// ESP32-C3 does NOT reliably provide `Serial`
static inline HardwareSerial& dbg_serial() {
#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32C3)
  return Serial0;
#else
  return Serial;
#endif
}

static inline uint32_t ms_now() { return millis(); }

static inline void log_header(const char* tag) {
  dbg_serial().printf("\n[%s]\n", tag ? tag : "");
}

static inline void log_line(const char* tag, const char* msg) {
  dbg_serial().printf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
}

} // namespace canary
