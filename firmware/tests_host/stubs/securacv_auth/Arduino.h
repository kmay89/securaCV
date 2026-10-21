/* Minimal Arduino stub for compiling the flagship's REAL
 * canary/lib/securacv_auth/src/securacv_auth.cpp under
 * test_securacv_auth.cpp: a millis() the test moves (32 bits wide, as on the
 * ESP32, so the backoff's arithmetic wraps where the device's does) and a
 * Serial that counts its lines and prints nothing (auth_load_or_derive()'s
 * one persist-failure line, and the console lines of the handlers cut from
 * securacv_network.cpp). securacv_auth.h includes <Arduino.h> for these
 * and the C string and stdio headers the library uses. */
#ifndef STUB_SECURACV_AUTH_ARDUINO_H
#define STUB_SECURACV_AUTH_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

inline uint32_t g_host_millis = 0;
inline uint32_t millis() { return g_host_millis; }

struct HostSerial {
  int lines = 0;
  void println(const char*) { lines++; }
  template <class... Args>
  void printf(const char*, Args...) { lines++; }
};
inline HostSerial Serial;

#endif
