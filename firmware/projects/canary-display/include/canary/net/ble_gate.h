#pragma once
#include <stddef.h>
#include <esp_heap_caps.h>

// Shared BLE heap gate for the display's passive-scan (chirp_scan.cpp) and
// central/GATT (fleet_link.cpp) paths.
//
// Minimum internal, DMA-capable SRAM headroom before standing the BT
// controller up. With WiFi already associated the two contend for the internal
// heap the controller draws from (PSRAM cannot back it), and starting it when
// that pool is thin is what logged "BLE_INIT: Malloc failed" and tripped the
// interrupt watchdog (the emi.c assert on the controller task) into a reboot
// loop. These mirror the WAP's host-tested guard (bt_defaults.h;
// firmware/LESSONS_LEARNED): the controller's largest single init allocation is
// ~30 KB contiguous — 0x7800, the very size in the "BLE assert emi.c 164 ...
// 00007800" panic — and the whole stack costs ~55-65 KB, so require a 48 KB
// contiguous block AND 96 KB total free, both measured on the INTERNAL|DMA
// pool. Below this we skip: the off-grid radio is the expendable decision — a
// live glass beats a boot-looping one.

namespace canary::net {

constexpr size_t BLE_MIN_FREE_BLOCK = 48 * 1024;
constexpr size_t BLE_MIN_TOTAL_FREE = 96 * 1024;

// True when the internal/DMA heap has room to bring the BT controller up.
// A thin pool here is the normal state mid-WiFi-reconnect, so callers treat a
// false as a skip-and-retry (re-check next window), not a hard failure.
inline bool ble_heap_ok() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >=
             BLE_MIN_FREE_BLOCK &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >=
             BLE_MIN_TOTAL_FREE;
}

}  // namespace canary::net
