/* Host stand-in for <Arduino.h> (the receipt-grant harness,
 * test_receipt_grant.cpp, sweep F289): the REAL api_auth.h and the handlers
 * cut out of canary_wap.ino over a clock the test moves and a Serial whose
 * printf lines are kept (stub_grant::serial_lines), so a test can read the
 * console line a strike prints and hold it to naming no address. */
#ifndef STUB_RECEIPT_GRANT_ARDUINO_H
#define STUB_RECEIPT_GRANT_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace stub_grant {
inline uint32_t now_ms = 1000;
// Every line Serial.printf() printed, as formatted (cut at 255 bytes).
inline std::vector<std::string> serial_lines;
}  // namespace stub_grant

inline uint32_t millis() { return stub_grant::now_ms; }

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
