// canary-local/emulator/shim/esp_heap_caps.h — PSRAM alloc → plain malloc.
// The dash's LVGL draw buffers ask for SPIRAM; wasm linear memory is all
// one heap, so the capability flag is advisory here.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)

static inline void* heap_caps_malloc(size_t size, uint32_t /*caps*/) {
  return malloc(size);
}
static inline void heap_caps_free(void* p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t /*caps*/) {
  return 4u * 1024u * 1024u;
}
static inline size_t heap_caps_get_largest_free_block(uint32_t /*caps*/) {
  return 2u * 1024u * 1024u;
}
