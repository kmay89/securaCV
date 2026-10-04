/* Arduino.h for the MQTT bridge host build (test_mqtt_reinit.cpp, sweep
 * F106): the REAL csi_mqtt.cpp over these stubs. millis() reads a clock the
 * test moves. delay() is where a task gives up the CPU (the handlers wait
 * for the loop task there), so it calls stub_mqtt::on_delay when a test
 * set one, to play another task's turn; by default it moves the clock. */
#ifndef STUB_MQTT_ARDUINO_H
#define STUB_MQTT_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

#define PROGMEM

struct StubSerial {
  bool quiet = true;
  void println(const char* s = "") { if (!quiet) printf("%s\n", s); }
  void print(const char* s) { if (!quiet) printf("%s", s); }
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

namespace stub_mqtt {
inline uint32_t now_ms = 1000;
inline unsigned delays = 0;
inline std::function<void(uint32_t ms)> on_delay;
}  // namespace stub_mqtt

inline uint32_t millis() { return stub_mqtt::now_ms; }
inline void delay(uint32_t ms) {
  ++stub_mqtt::delays;
  if (stub_mqtt::on_delay) {
    stub_mqtt::on_delay(ms);
  } else {
    stub_mqtt::now_ms += ms;
  }
}
inline void yield() {}

#endif
