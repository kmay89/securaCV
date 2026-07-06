/**
 * @file csi_mem.h
 * @brief Large-buffer allocator: prefer PSRAM, fall back to internal heap.
 *
 * The ESP32-S3's 320 KB internal DRAM bank is the scarce resource that
 * decides whether the BLE stack can start (~96 KB free needed); the 8 MB
 * PSRAM is not. Multi-KB buffers that are only ever touched from task
 * context (never from an ISR, never while the flash cache is disabled)
 * belong in PSRAM — every KB moved is a KB of internal heap back.
 *
 * csi_large_calloc() allocates zero-initialized memory from PSRAM when the
 * platform has it, and degrades in order:
 *   1. heap_caps_calloc(MALLOC_CAP_SPIRAM)  — ESP32 targets with PSRAM
 *   2. calloc()                             — no PSRAM (ESP32-C3), PSRAM
 *                                             exhausted, or host builds
 * Callers MUST handle a NULL return by disabling their feature (fail-safe),
 * never by dereferencing: on a C3 the internal fallback simply recreates
 * today's static footprint, and on a healthy S3 the PSRAM path always wins.
 *
 * Header-only and host-buildable (the heap_caps include is feature-tested),
 * so the canonical CSI library keeps compiling everywhere it does today.
 */

#ifndef CSI_MEM_H
#define CSI_MEM_H

#include <stddef.h>
#include <stdlib.h>

#if defined(__has_include)
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define CSI_MEM_HAS_HEAP_CAPS 1
#endif
#endif

static inline void* csi_large_calloc(size_t nbytes) {
#if defined(CSI_MEM_HAS_HEAP_CAPS)
  void* p = heap_caps_calloc(1, nbytes, MALLOC_CAP_SPIRAM);
  if (p) return p;
#endif
  return calloc(1, nbytes);
}

#endif /* CSI_MEM_H */
