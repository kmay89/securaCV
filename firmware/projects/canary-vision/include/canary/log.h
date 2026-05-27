#pragma once
#include <Arduino.h>

namespace canary {

// With ARDUINO_USB_CDC_ON_BOOT disabled for C3 (see common_esp32c3),
// Serial is UART0 on both C3 and S3 — no special case needed.
static inline HardwareSerial& dbg_serial() {
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
