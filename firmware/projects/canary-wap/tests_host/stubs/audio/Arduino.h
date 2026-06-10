// Host-test stub for Arduino.h — just enough surface for securacv_audio.cpp.
// millis() is backed by a test-controlled clock (see test_audio_cadence.cpp).
#ifndef HOST_STUB_ARDUINO_H
#define HOST_STUB_ARDUINO_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
unsigned long millis();
#endif
