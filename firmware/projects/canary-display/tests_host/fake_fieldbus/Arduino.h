// tests_host/fake_fieldbus/Arduino.h — the Arduino core as the display's
// fieldbus code uses it (src/playground/playground.cpp, src/io/rs485.cpp,
// src/io/can_bus.cpp, canary/log.h), for test_fieldbus_glue.cpp.
//
// Time: millis() reads a fake microsecond clock and moves it on by
// kMillisStepUs per call, so a loop that only polls millis() (the playground's
// inline reply wait) still reaches its deadline, as it does on the chip where
// each pass takes real time; delay(ms) moves it on by ms.
//
// Serial1: the members the code calls, with the signatures arduino-esp32
// 2.0.17 and 3.3.x both declare in cores/esp32/HardwareSerial.h —
// begin(baud, config, rxPin, txPin, ...), available(), read(),
// write(const uint8_t*, size_t), flush(). SERIAL_8N1 is the value
// esp32-hal-uart.h gives it. It records every byte written (the bus), every
// begin and flush, and an ordered op log; received bytes are queued with an
// arrival time and become available() once the clock passes it. A flush()
// hands the bytes written since the last flush to on_frame, which a test uses
// as the far end of the bus (a Modbus slave).
//
// Serial: the console. Everything printed is kept in `text`.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fake {
inline uint64_t now_us = 1000000;            // one second after boot
constexpr uint64_t kMillisStepUs = 50;       // one pass of a polling loop
inline uint64_t delay_calls = 0;
}  // namespace fake

inline uint32_t millis() {
  const uint32_t m = (uint32_t)(fake::now_us / 1000);
  fake::now_us += fake::kMillisStepUs;
  return m;
}

inline void delay(uint32_t ms) {
  fake::delay_calls++;
  fake::now_us += (uint64_t)ms * 1000;
}

#define SERIAL_8N1 0x800001c
#define SERIAL_8E1 0x800001e

struct FakeConsole {
  std::string text;
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text += buf;
    return n < 0 ? 0 : (size_t)n;
  }
};
inline FakeConsole Serial;

struct FakeUart {
  int begins = 0;
  unsigned long baud = 0;
  uint32_t config = 0;
  int rx_pin = -1;
  int tx_pin = -1;
  int flushes = 0;
  std::vector<uint8_t> tx;          // every byte written, in order
  std::vector<std::string> ops;     // "begin" / "read" / "write" / "flush"
  struct Rx {
    uint64_t at_us;
    uint8_t b;
  };
  std::deque<Rx> rx;
  size_t frame_start = 0;
  std::function<void(const std::vector<uint8_t>&)> on_frame;

  void begin(unsigned long b, uint32_t cfg = SERIAL_8N1, int8_t rxp = -1,
             int8_t txp = -1, bool invert = false,
             unsigned long timeout_ms = 20000UL,
             uint8_t rxfifo_full_thrhd = 120) {
    (void)invert;
    (void)timeout_ms;
    (void)rxfifo_full_thrhd;
    begins++;
    baud = b;
    config = cfg;
    rx_pin = rxp;
    tx_pin = txp;
    ops.push_back("begin");
  }
  int available() {
    int n = 0;
    for (const Rx& r : rx) {
      if (r.at_us > fake::now_us) break;
      n++;
    }
    return n;
  }
  int read() {
    if (rx.empty() || rx.front().at_us > fake::now_us) return -1;
    const uint8_t b = rx.front().b;
    rx.pop_front();
    ops.push_back("read");
    return b;
  }
  size_t write(const uint8_t* p, size_t n) {
    tx.insert(tx.end(), p, p + n);
    ops.push_back("write");
    return n;
  }
  void flush() {
    flushes++;
    ops.push_back("flush");
    const std::vector<uint8_t> frame(tx.begin() + (long)frame_start, tx.end());
    frame_start = tx.size();
    if (on_frame && !frame.empty()) on_frame(frame);
  }
  // Queue bytes that arrive `after_ms` from now, behind anything queued.
  void inject(const std::vector<uint8_t>& bytes, uint32_t after_ms) {
    uint64_t at = fake::now_us + (uint64_t)after_ms * 1000;
    if (!rx.empty() && rx.back().at_us > at) at = rx.back().at_us;
    for (uint8_t b : bytes) rx.push_back({at, b});
  }
};
inline FakeUart Serial1;
