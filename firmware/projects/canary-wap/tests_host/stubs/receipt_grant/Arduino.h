/* Host stand-in for <Arduino.h> (the receipt-grant harness,
 * test_receipt_grant.cpp, sweep F289): the REAL api_auth.h and the handlers
 * cut out of canary_wap.ino over a clock the test moves and a Serial whose
 * printf lines are kept (stub_grant::serial_lines), so a test can read the
 * console line a strike prints and hold it to naming no address. A test can
 * also arm stub_grant::millis_hook: every millis() call then runs it with
 * the call's index (counted from millis_calls, which the test zeroes) and
 * returns what it returns; a millis() inside the hook reads now_ms (sweep
 * F342: the receipt's tap step reads the clock between its peek() and its
 * take_exact(), so a hook on that call is another task acting between the
 * two). */
#ifndef STUB_RECEIPT_GRANT_ARDUINO_H
#define STUB_RECEIPT_GRANT_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

namespace stub_grant {
inline uint32_t now_ms = 1000;
// Every line Serial.printf() printed, as formatted (cut at 255 bytes).
inline std::vector<std::string> serial_lines;
inline std::function<uint32_t(unsigned)> millis_hook;
inline unsigned millis_calls = 0;
inline bool in_millis_hook = false;
}  // namespace stub_grant

inline uint32_t millis() {
  if (stub_grant::millis_hook && !stub_grant::in_millis_hook) {
    stub_grant::in_millis_hook = true;
    const uint32_t v = stub_grant::millis_hook(stub_grant::millis_calls++);
    stub_grant::in_millis_hook = false;
    return v;
  }
  return stub_grant::now_ms;
}

struct StubSerial {
  void println(const char* = "") {}
  void print(const char*) {}
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    stub_grant::serial_lines.emplace_back(line);
    return n;
  }
};
inline StubSerial Serial;

#endif
