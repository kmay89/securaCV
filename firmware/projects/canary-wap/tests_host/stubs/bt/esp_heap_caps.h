/* Host stand-in for <esp_heap_caps.h> (the Bluetooth channel host harness):
 * the internal heap ble_heap_guard measures before a NimBLE init. The test
 * sets how much there is (default: plenty). */
#ifndef STUB_BT_ESP_HEAP_CAPS_H
#define STUB_BT_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)

namespace host_sim {
inline size_t internal_largest_block = 200 * 1024;
inline size_t internal_free = 250 * 1024;
}  // namespace host_sim

inline size_t heap_caps_get_largest_free_block(unsigned) { return host_sim::internal_largest_block; }
inline size_t heap_caps_get_free_size(unsigned) { return host_sim::internal_free; }
inline void* heap_caps_calloc(size_t n, size_t sz, unsigned) { return calloc(n, sz); }

#endif
