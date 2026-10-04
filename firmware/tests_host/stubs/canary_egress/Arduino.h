/* Arduino.h for test_canary_event_egress.cpp: the canary's REAL
 * src/csi_event_egress.cpp and src/csi_event_log.cpp on the host. Serial
 * prints to stdout (quiet by default: the suite prints its own lines), and
 * millis() is a clock the test moves, so the backfill's pacing and the
 * egress's card wait are the test's to drive. psramFound()/ps_malloc() as
 * on a board without PSRAM. */
#ifndef STUB_CANARY_EGRESS_ARDUINO_H
#define STUB_CANARY_EGRESS_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct StubSerial {
  bool quiet = true;
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

inline uint32_t& stub_millis() {
  static uint32_t ms = 1000;
  return ms;
}
inline uint32_t millis() { return stub_millis(); }

inline bool psramFound() { return false; }
inline void* ps_malloc(size_t n) { return malloc(n); }

#endif
