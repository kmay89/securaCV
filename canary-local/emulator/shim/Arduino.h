// canary-local/emulator/shim/Arduino.h — browser-build Arduino surface.
//
// The emulator compiles the REAL canary-display firmware (main.cpp, the
// LVGL faces, the care/fleet/trust layers) to WebAssembly. This header
// stands in for arduino-esp32's Arduino.h at the exact API surface the
// display firmware actually touches — nothing speculative. Time is
// virtual (scenario-scalable), Serial streams into the page's serial
// panel, and LEDC writes become JS callbacks (backlight brightness /
// Web Audio chime). Everything above this line of silicon is the
// firmware's own bytes.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// arduino-esp32 core 2.x is the canonical PlatformIO line (common.ini);
// core_compat.h keys its LEDC/TWDT spelling off this.
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#ifndef PROGMEM
#define PROGMEM
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ── Virtual clock (emu_support.cpp) ─────────────────────────────────────
// millis() advances with wall time by default; the scenario API can scale
// it (time-lapse: watch staleness deadlines and the bird's anxiety decay
// in seconds instead of minutes) or step it. delay() yields to the
// browser event loop through Asyncify — the firmware's own pacing,
// including the splash storyboard's blocking pump(), runs unmodified.
uint32_t millis(void);
void delay(uint32_t ms);
uint32_t micros(void);

// ── GPIO: the BOOT/user button, and nothing else ────────────────────────
// The display firmware reads exactly one digital input (io/boot_button.h:
// tap = peek, double = lantern, hold = acknowledge). A browser has no
// button, so JS pushes the level in through emu_button(), the same way
// pointer events push touch — and digitalRead idles HIGH (released),
// since BOOT_BUTTON_ACTIVE is LOW on every board that carries one.
#define LOW           0x0
#define HIGH          0x1
#define INPUT         0x01
#define OUTPUT        0x03
#define INPUT_PULLUP  0x05
void pinMode(uint8_t pin, uint8_t mode);
int  digitalRead(uint8_t pin);
void digitalWrite(uint8_t pin, uint8_t value);
void emu_button(int down);

// ── LEDC (backlight PWM + chime tone), core-2.x channel spelling ────────
void ledcSetup(uint8_t channel, uint32_t freq_hz, uint8_t resolution_bits);
void ledcAttachPin(uint8_t pin, uint8_t channel);
void ledcWrite(uint8_t channel, uint32_t duty);
void ledcWriteTone(uint8_t channel, uint32_t freq_hz);

// ── Entropy plumbing (C-safe names; Arduino spellings are C++-only
//    overloads below, so LVGL's C units never see a clash with libc) ────
long emu_random_below(long max_exclusive);
long emu_random_between(long lo, long hi);
void emu_random_seed(unsigned long seed);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "WString.h"

inline long random(long max_exclusive) { return emu_random_below(max_exclusive); }
inline long random(long lo, long hi) { return emu_random_between(lo, hi); }
inline void randomSeed(unsigned long seed) { emu_random_seed(seed); }

// ── Serial → the page's serial-monitor panel ────────────────────────────
// The display's boot banner (common/boot) prints through this; the page
// shows the same USB-CDC console a user would see at 115200 baud.
class EmuSerial {
 public:
  void begin(unsigned long) {}
  void print(const char* s) { write_str(s ? s : ""); }
  void print(char c) { char b[2] = {c, 0}; write_str(b); }
  void print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); write_str(b); }
  void print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); write_str(b); }
  void print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); write_str(b); }
  void print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); write_str(b); }
  void print(float v) { char b[24]; snprintf(b, sizeof(b), "%g", (double)v); write_str(b); }
  void println() { write_str("\n"); }
  template <typename T> void println(T v) { print(v); write_str("\n"); }
  int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_str(buf);
    return n;
  }
  void flush() {}
  operator bool() const { return true; }

 private:
  static void write_str(const char* s);
};

extern EmuSerial Serial;

// ── ESP chip-info object (boot banner hardware scene) ───────────────────
class EmuESP {
 public:
  const char* getChipModel() { return "ESP32-S3 (emulated)"; }
  uint8_t getChipRevision() { return 0; }
  uint32_t getCpuFreqMHz() { return 240; }
  uint8_t getChipCores() { return 2; }
  uint32_t getFlashChipSize() { return 8u * 1024u * 1024u; }
  uint32_t getFreeHeap();
  uint32_t getMinFreeHeap() { return getFreeHeap() - 40 * 1024; }
  const char* getSdkVersion() { return "wasm-emu"; }
  void restart();
};

extern EmuESP ESP;

// configTzTime (FEATURE_SNTP): apply the POSIX TZ rule to libc so
// localtime_r speaks wall time; SNTP servers are ignored — the browser's
// clock is already "synced".
void configTzTime(const char* tz, const char* s1, const char* s2 = nullptr,
                  const char* s3 = nullptr);
#endif  // __cplusplus
