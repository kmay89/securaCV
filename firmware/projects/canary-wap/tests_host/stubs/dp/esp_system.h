/* esp_fill_random over a seeded host PRNG, for test_dp_budget.cpp. Not a
 * CSPRNG and not meant to be: the test checks budget enforcement and noise
 * scale, never the randomness source. */
#ifndef STUB_DP_ESP_SYSTEM_H
#define STUB_DP_ESP_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include <mutex>
#include <random>

inline void esp_fill_random(void* buf, size_t len) {
  static std::mt19937 rng(12345);
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  uint8_t* p = (uint8_t*)buf;
  for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(rng() & 0xFF);
}

#endif
