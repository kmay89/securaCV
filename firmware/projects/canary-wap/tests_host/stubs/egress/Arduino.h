/* Arduino.h for the events-egress host build (test_wap_event_egress.cpp):
 * the real csi_event_egress.cpp and csi_event_log.cpp over the fake card in
 * ../sd_fake. A superset of ../sd_fake/Arduino.h (Serial to stdout,
 * vTaskDelay counted) with a clock the test moves: millis() returns
 * stub_millis(), so the backfill's pacing (kSendIntervalMs) is the test's
 * to drive. Listed before ../sd_fake on the include path. */
#ifndef STUB_EGRESS_ARDUINO_H
#define STUB_EGRESS_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct StubSerial {
  bool quiet = false;
  void println(const char* s) { if (!quiet) printf("%s\n", s); }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (quiet) return 0;
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
};
inline StubSerial Serial;

inline void yield() {}

inline unsigned& stub_task_delays() {
  static unsigned n = 0;
  return n;
}
inline void vTaskDelay(uint32_t) { stub_task_delays()++; }

inline uint32_t& stub_millis() {
  static uint32_t ms = 1000;
  return ms;
}
inline uint32_t millis() { return stub_millis(); }

#endif
