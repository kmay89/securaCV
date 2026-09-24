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
#include <errno.h>
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
  whb::Open open(uint32_t* size) {
    errno = 0;
    f = SD.open("/WITNESS/records.jsonl", FILE_READ);
    if (!f) {
      // The Arduino FS call folds "no such file" and "the card did not
      // answer" into one false. The VFS underneath (the core's vfs_api.cpp
      // tries stat(), then opendir()) leaves FATFS's errno: ENOENT only for
      // a path that is not there, EIO / ENODEV for a card that failed,
      // ENFILE with no free handle. Only a definite ENOENT is an empty
      // history; anything else, errno 0 included (the SD object had no
      // mount point), is an I/O error the client is told about.
      return errno == ENOENT ? whb::Open::MISSING : whb::Open::FAILED;
    }
    *size = (uint32_t)f.size();
    return whb::Open::OK;
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
  // The budget runs on millis() (esp_timer), so a delay that overruns
  // because a higher-priority task held the core counts as the time it took.
  const whb::Response* p = whb::wait(
      &s_slot, g, whb::WAIT_MS, whb::WAIT_STEP_MS,
      []() { return (uint32_t)millis(); },
      [](uint32_t ms) {
        const TickType_t t = pdMS_TO_TICKS(ms);
        vTaskDelay(t > 0 ? t : 1);
      });
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
