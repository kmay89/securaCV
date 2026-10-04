// tests_host/fake_lvgl9/esp_heap_caps.h — ESP-IDF's capability allocator,
// recorded: the test reads every request ui/lvgl_port.cpp made (its size,
// its tier and whether it was granted) and can make a tier, every tier, or
// any one request by its place in the order refuse.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <vector>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

namespace fake_heap {
struct Alloc { size_t bytes; uint32_t caps; };
struct Attempt { size_t bytes; uint32_t caps; bool granted; };
// The requests granted, in order.
inline std::vector<Alloc>& allocs() { static std::vector<Alloc> a; return a; }
// Every request, granted or refused, in order.
inline std::vector<Attempt>& attempts() { static std::vector<Attempt> a; return a; }
inline bool& refuse_spiram() { static bool v = false; return v; }
inline bool& refuse_all() { static bool v = false; return v; }
// Requests refused by their 0-based place among every heap_caps_malloc call
// since reset() — e.g. {1} grants the draw buffer and refuses the request
// that follows it, whatever tier either asks for.
inline std::vector<size_t>& refuse_calls() { static std::vector<size_t> v; return v; }
inline void reset() {
  allocs().clear();
  attempts().clear();
  refuse_calls().clear();
  refuse_spiram() = false;
  refuse_all() = false;
}
}  // namespace fake_heap

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
  const size_t call = fake_heap::attempts().size();
  const auto& script = fake_heap::refuse_calls();
  const bool refused = fake_heap::refuse_all() ||
                       ((caps & MALLOC_CAP_SPIRAM) && fake_heap::refuse_spiram()) ||
                       std::find(script.begin(), script.end(), call) != script.end();
  fake_heap::attempts().push_back({size, caps, !refused});
  if (refused) return nullptr;
  fake_heap::allocs().push_back({size, caps});
  return malloc(size);
}
inline void heap_caps_free(void* p) { free(p); }
