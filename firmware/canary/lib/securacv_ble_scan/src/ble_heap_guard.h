/*
 * SecuraCV Canary WAP — BLE init heap guard
 *
 * Every NimBLEDevice::init() call site consults this before bringing the BLE
 * controller up. The controller allocates a ~30 KB contiguous internal block
 * at init; on a build with PSRAM disabled the internal heap is too small/
 * fragmented once WiFi + HTTP are running, the malloc fails, and the controller
 * ASSERTS (it does not return an error) — panicking the device into a boot
 * loop that safe mode can't escape. Checking free memory first lets BLE degrade
 * (radio off) instead of bricking. The threshold + decision live in the
 * Arduino-free, host-tested bt_defaults.h; this header is only the glue that
 * reads the live heap.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BLE_HEAP_GUARD_H
#define SECURACV_BLE_HEAP_GUARD_H

#include <Arduino.h>
#include "esp_heap_caps.h"
#include "bt_defaults.h"

namespace ble_heap_guard {

// Largest contiguous internal, DMA-capable free block — the memory pool the
// BLE controller allocates from at init. PSRAM is deliberately excluded: the
// controller cannot use it.
inline size_t largest_internal_block() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

// Total free internal memory — the whole stack's ~55-65 KB spend must leave
// operating margin for everything else (see bt_defaults::MIN_INIT_TOTAL_FREE).
inline size_t total_internal_free() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

// True when the BLE stack can be brought up without OOM-panicking AND without
// starving the rest of the system. When false, the caller MUST skip
// NimBLEDevice::init() and leave the radio off. Optionally reports the
// measured values so the caller can log them.
inline bool can_init(size_t* out_largest_block = nullptr,
                     size_t* out_total_free = nullptr) {
  const size_t block = largest_internal_block();
  const size_t total = total_internal_free();
  if (out_largest_block) *out_largest_block = block;
  if (out_total_free)    *out_total_free = total;
  return bt_defaults::init_has_headroom(block, total);
}

}  // namespace ble_heap_guard

#endif  // SECURACV_BLE_HEAP_GUARD_H
