// tests_host/fake_lvgl9/esp_heap_caps.h — ESP-IDF's capability allocator,
// recorded: the test reads which buffers ui/lvgl_port.cpp asked for, from
// which tier and how large, and can make a tier (or every tier) refuse.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#include <vector>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

namespace fake_heap {
struct Alloc { size_t bytes; uint32_t caps; };
inline std::vector<Alloc>& allocs() { static std::vector<Alloc> a; return a; }
inline bool& refuse_spiram() { static bool v = false; return v; }
inline bool& refuse_all() { static bool v = false; return v; }
inline void reset() { allocs().clear(); refuse_spiram() = false; refuse_all() = false; }
}  // namespace fake_heap

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
  if (fake_heap::refuse_all()) return nullptr;
  if ((caps & MALLOC_CAP_SPIRAM) && fake_heap::refuse_spiram()) return nullptr;
  fake_heap::allocs().push_back({size, caps});
  return malloc(size);
}
inline void heap_caps_free(void* p) { free(p); }
