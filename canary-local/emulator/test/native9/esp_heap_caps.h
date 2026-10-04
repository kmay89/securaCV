// canary-local/emulator/test/native9/esp_heap_caps.h — ESP-IDF's capability
// allocator for glass_turn_lvgl9.sh (F225): plain malloc, with one switch the
// test flips to refuse every request after the first N, so a glass whose turn
// buffer cannot be had (lvgl_port_init's second request) is rendered through
// the real LVGL too. The host test (tests_host/fake_lvgl9/esp_heap_caps.h)
// holds which tier each request asks for; this file does not.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

namespace native9 {
// Requests granted before every later one is refused (-1: none refused).
inline long& heap_grant_limit() { static long v = -1; return v; }
inline long& heap_requests() { static long v = 0; return v; }
}  // namespace native9

inline void* heap_caps_malloc(size_t size, uint32_t /*caps*/) {
  const long n = native9::heap_requests()++;
  const long limit = native9::heap_grant_limit();
  if (limit >= 0 && n >= limit) return nullptr;
  return malloc(size);
}
inline void heap_caps_free(void* p) { free(p); }
