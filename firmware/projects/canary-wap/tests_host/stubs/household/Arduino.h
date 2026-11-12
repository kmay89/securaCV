/* Arduino for test_household_rpa.cpp: the sketch's real household.cpp over
 * the real nvs_store.h. Serial and String are stubs/nvs_store's (what
 * nvs_store.h and the first-boot Preferences fake use); millis() is a clock
 * the suite sets, for the enrollment window and the last-seen stamp. */
#ifndef STUB_HOUSEHOLD_ARDUINO_H
#define STUB_HOUSEHOLD_ARDUINO_H

#include "../nvs_store/Arduino.h"

inline uint32_t g_host_millis = 1000;
inline uint32_t millis() { return g_host_millis; }

#endif
