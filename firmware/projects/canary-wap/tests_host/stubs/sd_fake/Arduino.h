/* Minimal Arduino stub for host-compiling csi_event_log.cpp over the fake
 * SD card in SD.h next to this file (test_csi_event_log_load.cpp). Serial
 * prints to stdout so a suite's log shows what the loader said. */
#ifndef STUB_SD_FAKE_ARDUINO_H
#define STUB_SD_FAKE_ARDUINO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct StubSerial {
  void println(const char* s) { printf("%s\n", s); }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
};
static StubSerial Serial;

inline void yield() {}

/* The real Arduino.h brings FreeRTOS in. vTaskDelay() counts, so a test can
 * see the boot reload let the idle task run (test_csi_event_log_load.cpp). */
inline unsigned& stub_task_delays() {
  static unsigned n = 0;
  return n;
}
inline void vTaskDelay(uint32_t) { stub_task_delays()++; }

#endif
