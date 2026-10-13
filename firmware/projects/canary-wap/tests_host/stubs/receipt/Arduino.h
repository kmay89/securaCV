/* Arduino.h for the receipt-session suite (test_receipt_session.cpp, sweep
 * F289): api_auth.h's lockout, the session store's TTL and the claim's TTL
 * read millis(), a clock the test moves. Serial is silent. */
#ifndef STUB_RECEIPT_ARDUINO_H
#define STUB_RECEIPT_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROGMEM

struct StubReceiptSerial {
  void println(const char* = "") {}
  void print(const char*) {}
  int printf(const char*, ...) __attribute__((format(printf, 2, 3))) { return 0; }
};
inline StubReceiptSerial Serial;

namespace stub_receipt {
inline uint32_t now_ms = 1000;
}  // namespace stub_receipt

inline uint32_t millis() { return stub_receipt::now_ms; }

#endif
