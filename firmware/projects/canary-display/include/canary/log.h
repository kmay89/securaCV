#pragma once
#include <Arduino.h>

namespace canary {

// Board-dependent console type: UART0 HardwareSerial on C3 (CDC_ON_BOOT
// disabled, see common_esp32c3), HWCDC on S3 (native USB CDC).
// decltype picks the right one (C++11-safe — the framework compiles some
// TUs below C++14, so a deduced `auto&` return is not available here);
// callers only use the shared Print interface.
static inline decltype(Serial)& dbg_serial() {
  return Serial;
}

static inline uint32_t ms_now() { return millis(); }

static inline void log_header(const char* tag) {
  dbg_serial().printf("\n[%s]\n", tag ? tag : "");
}

// Optional second sink: the web mirror's browser "serial monitor"
// (glass_web) captures the same lines the cable would show, so bench
// triage works from a phone. nullptr = serial only. Defined in
// glass_web.cpp; set once at init on the loop task.
using LogSink = void (*)(const char* tag, const char* msg);
extern LogSink g_log_sink;

static inline void log_line(const char* tag, const char* msg) {
  dbg_serial().printf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
  if (g_log_sink) g_log_sink(tag, msg);
}

} // namespace canary
