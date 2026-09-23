/*
 * SecuraCV Canary — Timeline history from the card (repo sweep F35)
 *
 * Thin ESP glue over firmware/common/witness/witness_history_bridge.h; see
 * securacv_witness_history.h for the contract.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_witness_history.h"

#if FEATURE_SD_STORAGE

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include "securacv_storage.h"

namespace whb = witness_history_bridge;

// The one request slot and the loop's read buffer (~3.2 KiB + 1 KiB of
// .bss). Zero-initialized storage is the bridge's idle state.
static whb::Slot s_slot;
static char s_read_buf[whb::READ_LEN];

namespace {

// The SD side of one loop pass. Only witness_history_service() — the loop
// task — constructs one, so every call below runs where the storage contract
// allows it.
struct CardIo {
  File f;

  whb::Card card() {
    // While a background mount runs, its worker owns the SD object: not even
    // a probe (storage_mount_in_flight()'s contract).
    if (storage_mount_in_flight()) return whb::Card::BUSY;
    return storage_is_mounted() ? whb::Card::READY : whb::Card::ABSENT;
  }
  uint32_t card_token() { return storage_mount_generation(); }
  bool open(uint32_t* size) {
    f = SD.open("/WITNESS/records.jsonl", FILE_READ);
    if (!f) return false;
    *size = (uint32_t)f.size();
    return true;
  }
  size_t read(uint32_t off, char* buf, size_t len) {
    if (!f.seek(off)) return 0;
    return f.read((uint8_t*)buf, len);
  }
  void close() {
    if (f) f.close();
  }
};

}  // namespace

WitnessHistoryWait witness_history_request(const whb::Request& req,
                                           const whb::Response** page,
                                           uint32_t* gen) {
  const uint32_t g = whb::begin(&s_slot, req);
  if (g == 0) return WitnessHistoryWait::BUSY;
  const whb::Response* p = whb::wait(&s_slot, g, whb::WAIT_MS, whb::WAIT_STEP_MS,
                                     [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });
  if (p == nullptr) {
    whb::end(&s_slot, g, /*gave_up=*/true);
    return WitnessHistoryWait::TIMEOUT;
  }
  *page = p;
  *gen = g;
  return WitnessHistoryWait::PAGE;
}

void witness_history_release(uint32_t gen) { whb::end(&s_slot, gen, /*gave_up=*/false); }

void witness_history_service() {
  CardIo io;
  whb::service(&s_slot, io, s_read_buf);
}

#endif  // FEATURE_SD_STORAGE
