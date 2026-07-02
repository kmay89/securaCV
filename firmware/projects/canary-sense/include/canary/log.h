#pragma once
#include <Arduino.h>

namespace canary {

// Board-dependent console type: the ESP32-C6 routes `Serial` to the USB
// Serial/JTAG peripheral (like the C3). decltype picks the concrete type
// (C++11-safe); callers only use the shared Print interface.
static inline decltype(Serial)& dbg_serial() {
  return Serial;
}

static inline uint32_t ms_now() { return millis(); }

static inline void log_header(const char* tag) {
  dbg_serial().printf("\n[%s]\n", tag ? tag : "");
}

static inline void log_line(const char* tag, const char* msg) {
  dbg_serial().printf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
}

} // namespace canary
