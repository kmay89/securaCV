/* Arduino.h for the receipt-session suite (test_receipt_session.cpp, sweep
 * F289): api_auth.h's lockout and the claim's TTL read millis(), and the
 * sessions and the pair tokens read esp_timer_get_time() (stubs/receipt/
 * esp_timer.h), both off one 64-bit clock the test moves. millis() is the
 * low 32 bits of it, as on Arduino-ESP32, so it wraps every 2^32 ms where
 * esp_timer does not. Serial is silent. */
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
inline uint64_t now_ms = 1000;   /* milliseconds since boot */
}  // namespace stub_receipt

inline uint32_t millis() { return (uint32_t)stub_receipt::now_ms; }

#endif
