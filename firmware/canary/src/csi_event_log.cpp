/*
 * SecuraCV Canary — SD event log adapter (backlog F37). See header.
 */

#include "csi_event_log.h"
#include "canary_config.h"

#if FEATURE_CSI && FEATURE_HA_MQTT

#include <Arduino.h>
#include <string.h>

#if FEATURE_SD_STORAGE
#include <FS.h>
#include <SD.h>

#include "securacv_storage.h"
#include "securacv_witness.h"  // log_health

#if FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif
#endif

namespace csi_event_log {

#if FEATURE_SD_STORAGE

namespace {

using csi_event_log_line::kDirPath;
using csi_event_log_line::kLogPath;

/* The retention rewrite's scratch copy, next to the log so the commit is a
 * rename inside one directory (canary-wap's crash model). */
constexpr const char* kTmpPath = "/EVENTS/today.ndjson.tmp";
/* One line: the fingerprint of the witness key the log belongs to. */
constexpr const char* kOwnerPath = "/EVENTS/owner";

bool     s_open = false;      // the log is usable now
bool     s_reported = false;  // what the last poll told the planner
uint32_t s_gen = 0;           // the mount generation last evaluated
uint32_t s_size = 0;          // the log's size; this file is its only writer
bool     s_needs_seal = false;  // the log ends in a torn line: '\n' first
uint32_t s_tail_id = 0;
/* Static, not on the loop task's stack: the truncation copy buffer, which
 * also holds the tail read at open (the two never overlap). */
uint8_t  s_stream[csi_event_backfill::kTailRead];

/* A crash inside the retention rewrite leaves one of three states; resolve
 * it before the log is used (canary-wap csi_event_log.cpp, ported):
 *   log + tmp  -> the rewrite never committed: the tmp is partial, drop it;
 *   tmp only   -> the old log was removed but the rename never ran: promote;
 *   log only   -> nothing to do. */
void reconcile_truncate_remnants() {
  const bool has_log = SD.exists(kLogPath);
  const bool has_tmp = SD.exists(kTmpPath);
  if (has_log && has_tmp) {
    if (SD.remove(kTmpPath)) Serial.println("[EVT-LOG] cleared stale truncate scratch (.tmp)");
  } else if (!has_log && has_tmp) {
    if (SD.rename(kTmpPath, kLogPath)) {
      Serial.println("[EVT-LOG] recovered events from an interrupted truncate");
    } else {
      SD.remove(kTmpPath);
      Serial.println("[EVT-LOG] truncate recovery rename failed - .tmp dropped");
    }
  }
}

/* Past kMaxBytes, drop the oldest quarter (to the next line start) by
 * streaming the survivors into the tmp file and renaming it over the log.
 * Returns the bytes dropped from the head (0 = nothing dropped) and sets
 * s_size. `*broken` = the old log is gone but the rename failed: the
 * survivors sit in the tmp file, which only a remount's reconcile may
 * promote, so the log must not be written again this mount. */
uint32_t head_truncate(bool* broken) {
  File f = SD.open(kLogPath, FILE_READ);
  if (!f) return 0;
  const size_t sz = f.size();
  if (sz < kMaxBytes) {
    f.close();
    return 0;
  }
  if (!f.seek(sz / 4)) {
    f.close();
    return 0;
  }
  while (f.available()) {
    if (f.read() == '\n') break;
  }
  const uint32_t cut = (uint32_t)f.position();

  File w = SD.open(kTmpPath, FILE_WRITE);
  if (!w) {
    f.close();
    return 0;
  }
  bool ok = true;
#if FEATURE_WATCHDOG
  size_t since_feed = 0;  // bytes copied since the last watchdog feed
#endif
  while (f.available()) {
    const int n = f.read(s_stream, sizeof(s_stream));
    if (n <= 0) break;
    if (w.write(s_stream, (size_t)n) != (size_t)n) {
      ok = false;
      break;
    }
#if FEATURE_WATCHDOG
    // ~190 KB of copy on the loop task: feed its watchdog every 16 KiB. The
    // count restarts at each feed. A running total tested for a multiple
    // of 16 KiB would never hit one again after a single short read.
    since_feed += (size_t)n;
    if (since_feed >= 16u * sizeof(s_stream)) {
      esp_task_wdt_reset();
      since_feed = 0;
    }
#endif
  }
  f.close();
  w.close();
  if (!ok) {
    SD.remove(kTmpPath);
    return 0;
  }
  SD.remove(kLogPath);
  if (!SD.rename(kTmpPath, kLogPath)) {
    Serial.println("[EVT-LOG] truncate rename failed - log closed until the next mount");
    *broken = true;
    return 0;
  }
  s_size = (uint32_t)(sz - cut);
  return cut;
}

void read_owner(char* out, size_t cap) {
  out[0] = '\0';
  if (!SD.exists(kOwnerPath)) return;
  File f = SD.open(kOwnerPath, FILE_READ);
  if (!f) return;
  const size_t n = f.read((uint8_t*)out, cap - 1);
  out[n] = '\0';
  f.close();
}

bool write_owner(const char* fp) {
  File w = SD.open(kOwnerPath, FILE_WRITE);
  if (!w) return false;
  const size_t len = strlen(fp);
  const bool ok = w.write((const uint8_t*)fp, len) == len &&
                  w.write((const uint8_t*)"\n", 1) == 1;
  w.close();
  return ok;
}

/* Evaluate a newly mounted card: whose log it is, then its size and last
 * id. Nothing is written to a card whose log is not this device's. */
bool open_card(const char* owner_fp) {
  s_size = 0;
  s_tail_id = 0;
  s_needs_seal = false;

  const bool has_dir = SD.exists(kDirPath);
  const bool has_log = has_dir && (SD.exists(kLogPath) || SD.exists(kTmpPath));
  char recorded[48];
  recorded[0] = '\0';
  if (has_dir) read_owner(recorded, sizeof(recorded));

  switch (csi_event_backfill::owner_verdict(has_log, recorded, owner_fp)) {
    case csi_event_backfill::Owner::kForeign:
      Serial.println("[EVT-LOG] the card's event log is not this device's - left untouched");
      log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
                 "SD event log belongs to another device - not used",
                 "events still publish; no card backfill until the card is cleared");
      return false;
    case csi_event_backfill::Owner::kClaim:
      if ((!has_dir && !SD.mkdir(kDirPath)) || !write_owner(owner_fp)) {
        Serial.println("[EVT-LOG] could not claim /EVENTS - event log off for this mount");
        return false;
      }
      break;
    case csi_event_backfill::Owner::kOurs:
      break;
  }

  reconcile_truncate_remnants();
  if (!SD.exists(kLogPath)) return true;  // created by the first append

  File f = SD.open(kLogPath, FILE_READ);
  if (!f) return false;
  const size_t sz = f.size();
  s_size = (uint32_t)sz;
  if (sz > 0) {
    const size_t want = (sz < sizeof(s_stream)) ? sz : sizeof(s_stream);
    if (f.seek(sz - want)) {
      const size_t got = f.read(s_stream, want);
      if (got > 0) {
        s_needs_seal = (s_stream[got - 1] != '\n');
        s_tail_id = csi_event_backfill::last_line_id(
            reinterpret_cast<const char*>(s_stream), got);
      }
    }
  }
  f.close();
  return true;
}

/* The storage manager's rule: a mounted card, and no background mount
 * attempt in flight (it owns the SD object while it runs). poll() latches
 * it once per pump pass, and append() and read_at() ask it again on every
 * call. A run of failed writes (storage_note_write_failure) marks the card
 * lost at once, mid-pass, and the rest of the pass must not touch it. */
bool card_usable() {
  return storage_is_mounted() && !storage_mount_in_flight();
}

}  // namespace

Card poll(const char* owner_fp, uint32_t* size, uint32_t* tail_id) {
  const bool usable = card_usable();
  bool opened_now = false;
  if (!usable) {
    s_open = false;
  } else if (storage_mount_generation() != s_gen) {
    // A card mounted since the last look (or the first look this boot).
    // Evaluated once per mount: a refused or broken log stays closed until
    // the card is remounted.
    s_gen = storage_mount_generation();
    s_open = open_card(owner_fp);
    opened_now = s_open;
  }
  if (opened_now) {
    s_reported = true;
    if (size) *size = s_size;
    if (tail_id) *tail_id = s_tail_id;
    Serial.printf("[EVT-LOG] %s open: %lu bytes, last id %lu\n", kLogPath,
                  (unsigned long)s_size, (unsigned long)s_tail_id);
    return Card::kOpened;
  }
  if (s_reported && !s_open) {
    s_reported = false;
    return Card::kClosed;
  }
  return Card::kUnchanged;
}

csi_event_backfill::AppendResult append(const char* line, size_t len) {
  csi_event_backfill::AppendResult r = {false, s_size, 0};
  if (!s_open || !card_usable() || !line || len == 0) return r;

  if (s_size >= kMaxBytes) {
    bool broken = false;
    const uint32_t cut = head_truncate(&broken);
    if (broken) {
      s_open = false;  // the next poll reports it closed
      r.size = s_size;
      return r;
    }
    r.cut = cut;
  }

  // Card-op failures below feed the storage manager's consecutive-failure
  // count, as the witness appends do: past its threshold the card is marked
  // lost and remounted by the loop's periodic check.
  File f = SD.open(kLogPath, FILE_APPEND);
  if (!f) {
    storage_note_write_failure();
    r.size = s_size;
    return r;
  }
  if (s_needs_seal) {
    if (f.write((const uint8_t*)"\n", 1) != 1) {
      f.close();
      storage_note_write_failure();
      r.size = s_size;
      return r;
    }
    s_size += 1;
    s_needs_seal = false;
  }
  const size_t wrote = f.write((const uint8_t*)line, len);
  f.close();
  s_size += (uint32_t)wrote;
  r.size = s_size;
  if (wrote != len) {
    if (wrote > 0) s_needs_seal = true;  // a torn line now ends the log
    storage_note_write_failure();
    return r;
  }
  storage_note_write_success();
  r.ok = true;
  return r;
}

size_t read_at(uint32_t off, char* buf, size_t cap) {
  if (!s_open || !card_usable() || !buf || cap == 0) return 0;
  File f = SD.open(kLogPath, FILE_READ);
  if (!f) return 0;
  size_t got = 0;
  if (f.seek(off)) got = f.read((uint8_t*)buf, cap);
  f.close();
  return got;
}

#else  // !FEATURE_SD_STORAGE — no card on this build: the log is never open

Card poll(const char*, uint32_t*, uint32_t*) { return Card::kUnchanged; }
csi_event_backfill::AppendResult append(const char*, size_t) { return {false, 0, 0}; }
size_t read_at(uint32_t, char*, size_t) { return 0; }

#endif  // FEATURE_SD_STORAGE

}  // namespace csi_event_log

#endif  // FEATURE_CSI && FEATURE_HA_MQTT
