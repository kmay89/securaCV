/*
 * SecuraCV — boot guard: the device half of boot_policy.h (ESP-IDF / Arduino).
 *
 * boot_policy.h is the pure, host-tested decision. This header is the thin
 * glue that feeds it real inputs and persists its answer:
 *
 *   · the counter lives in NVS (namespace "scv_boot", key "unhealthy", u16),
 *     so it survives a brownout or a full power cut — RTC memory would not;
 *   · "is the running image confirmed?" is read from the OTA data partition:
 *     anything but ESP_OTA_IMG_PENDING_VERIFY is confirmed (a serial-flashed
 *     or factory image has no pending state, so it counts as confirmed);
 *   · "is this the image that left the count behind?" compares the running
 *     app's ELF SHA-256 prefix with the one stored beside the counter, so an
 *     OTA install, an A/B rollback or a re-flash of another build starts
 *     clean (boot_policy::carry_count).
 *
 * Call order (see boot_policy.h, "THE COUNTER'S LIFECYCLE"):
 *   1. bootguard::begin() very early in setup(), before risky init. It
 *      persists the incremented count BEFORE returning, so a hang during init
 *      is still counted. If it says BootMode::SafeMode, do not run normal
 *      init.
 *   2. bootguard::mark_healthy() once boot_policy::healthy_reached() says so
 *      (or before a deliberate restart / deep sleep from the running loop).
 *   3. bootguard::fresh_image_reset() just before rebooting into a newly
 *      installed OTA image; bootguard::operator_clear() from the safe-mode
 *      console's "clear & retry".
 *
 * Failure stance: if NVS cannot be opened the guard cannot count, and it says
 * so (Status::nvs_ok == false) and returns Normal. A device whose NVS is gone
 * has bigger problems than a crash loop, and refusing to boot on a storage
 * error would turn a recoverable fault into a brick.
 *
 * Wired in: firmware/canary/src/main.cpp (compile-checked, not bench-verified).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BOOT_GUARD_H
#define SECURACV_BOOT_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "nvs.h"

#include "health/boot_policy.h"

namespace bootguard {

constexpr const char* kNvsNamespace = "scv_boot";
constexpr const char* kNvsKeyCount  = "unhealthy";
constexpr const char* kNvsKeyImage  = "image";   // running image's ELF SHA-256, first 8 bytes
constexpr size_t      kImageIdLen   = 8;

struct Status {
  uint16_t prev_count;        // the count this boot found
  uint16_t count;             // the count this boot persisted
  bool     image_confirmed;   // false while the running image is PENDING_VERIFY
  bool     nvs_ok;            // false if the counter could not be read/written
  bootpolicy::BootMode mode;  // what this boot must do
};

// True unless the running OTA image is still awaiting confirmation.
inline bool image_confirmed() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return true;
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) != ESP_OK) return true;
  return st != ESP_OTA_IMG_PENDING_VERIFY;
}

// Reads the persisted count. Returns false if NVS could not be opened; a
// missing key is a first boot (count 0) and returns true.
inline bool load_count(uint16_t* out) {
  *out = 0;
  nvs_handle_t h;
  const esp_err_t open_err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (open_err != ESP_OK) {
    // NVS_READONLY on a namespace that was never written reports
    // ESP_ERR_NVS_NOT_FOUND — that is a first boot, not a failure.
    return open_err == ESP_ERR_NVS_NOT_FOUND;
  }
  uint16_t v = 0;
  const esp_err_t err = nvs_get_u16(h, kNvsKeyCount, &v);
  nvs_close(h);
  if (err == ESP_OK) *out = v;
  return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

// Persists the count; skips the write when the value is already there, so a
// good device's steady state costs one write per boot and one per healthy.
inline bool store_count(uint16_t n) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  uint16_t cur = 0;
  esp_err_t err = nvs_get_u16(h, kNvsKeyCount, &cur);
  if (err == ESP_OK && cur == n) {
    nvs_close(h);
    return true;
  }
  err = nvs_set_u16(h, kNvsKeyCount, n);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

// The running image's identity: the first bytes of its ELF SHA-256, read from
// the app descriptor in flash. False if it cannot be read.
inline bool running_image_id(uint8_t out[kImageIdLen]) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return false;
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(running, &desc) != ESP_OK) return false;
  memcpy(out, desc.app_elf_sha256, kImageIdLen);
  return true;
}

// Is this the image that left the persisted count behind? Records the running
// image's id when it is not, so the answer flips exactly once per new image.
// An unreadable id (either side) reads as "same image": the count is kept,
// which is the conservative direction for a crash-loop counter.
inline bool same_image_as_last_boot() {
  uint8_t now_id[kImageIdLen];
  if (!running_image_id(now_id)) return true;
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return true;
  uint8_t was_id[kImageIdLen] = {0};
  size_t len = sizeof(was_id);
  const esp_err_t err = nvs_get_blob(h, kNvsKeyImage, was_id, &len);
  const bool same = (err == ESP_OK && len == kImageIdLen &&
                     memcmp(was_id, now_id, kImageIdLen) == 0);
  if (!same) {
    if (nvs_set_blob(h, kNvsKeyImage, now_id, kImageIdLen) == ESP_OK) nvs_commit(h);
  }
  nvs_close(h);
  // A first-ever boot (no id stored yet) is a fresh image too.
  return same;
}

// Step 1. Count this boot, persist it, and say whether to run normally.
inline Status begin(uint16_t threshold = bootpolicy::kDefaultSafeModeThreshold) {
  Status s{};
  s.image_confirmed = image_confirmed();
  uint16_t prev = 0;
  s.nvs_ok = load_count(&prev);
  s.prev_count = prev;
  if (!s.nvs_ok) {
    s.count = 0;
    s.mode = bootpolicy::BootMode::Normal;
    return s;
  }
  prev = bootpolicy::carry_count(prev, same_image_as_last_boot());
  const bootpolicy::Decision d = bootpolicy::decide(prev, s.image_confirmed, threshold);
  s.nvs_ok = store_count(d.persist_count);
  s.count = d.persist_count;
  s.mode = d.mode;
  return s;
}

// Step 2 and the two named resets. Same value, different reasons, so the
// call site says why (boot_policy.h keeps the aliases for exactly this).
inline bool mark_healthy()       { return store_count(bootpolicy::kHealthyReset); }
inline bool fresh_image_reset()  { return store_count(bootpolicy::kFreshImageReset); }
inline bool operator_clear()     { return store_count(bootpolicy::kOperatorClearReset); }

}  // namespace bootguard

#endif  // SECURACV_BOOT_GUARD_H
