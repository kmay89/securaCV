#pragma once
#include <Arduino.h>

namespace canary {

// Board-dependent console type: UART0 HardwareSerial on C3 (CDC_ON_BOOT
// disabled, see common_esp32c3), HWCDC on S3 (native USB CDC). auto&
// deduces the right one; callers only use the shared Print interface.
static inline auto& dbg_serial() {
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
