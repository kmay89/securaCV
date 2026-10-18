/* Host stand-in for <Arduino.h> (the receipt-grant harness,
 * test_receipt_grant.cpp, sweep F289): the REAL api_auth.h and the handlers
 * cut out of canary_wap.ino over a clock the test moves and a Serial that
 * says nothing. */
#ifndef STUB_RECEIPT_GRANT_ARDUINO_H
#define STUB_RECEIPT_GRANT_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace stub_grant {
inline uint32_t now_ms = 1000;
}  // namespace stub_grant

inline uint32_t millis() { return stub_grant::now_ms; }

struct StubSerial {
  void println(const char* = "") {}
  void print(const char*) {}
  int printf(const char*, ...) __attribute__((format(printf, 2, 3))) { return 0; }
};
inline StubSerial Serial;

#endif
