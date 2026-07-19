// src/glass_settings.cpp — runtime screen settings persistence. See header.
#include <config.h>
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "canary/glass_settings.h"
#include "canary/log.h"

namespace canary::glass {

namespace {

constexpr const char* STORE_NS = "scv-glass";

// Versioned blob: magic + version + size gate the load, so a layout change
// (or corrupted flash) degrades to defaults instead of garbage settings.
struct Blob {
  uint16_t magic;    // 'S'<<8|'G'
  uint8_t  version;
  uint8_t  size;     // sizeof(Blob) at write time
  Settings s;
};
constexpr uint16_t BLOB_MAGIC   = 0x5347;
constexpr uint8_t  BLOB_VERSION = 2;  // v2: +character (v1 migrates below)

// Frozen v1 layout (pre-Character). Kept verbatim so a v1 blob migrates
// field-for-field instead of being rejected — an upgrade must never cost
// a user their night hours or the glow they tuned (review catch).
struct SettingsV1 {
  uint8_t  day_pct;
  uint8_t  night_screen;
  uint8_t  red_shift;
  uint8_t  peek_s;
  uint8_t  night_start_hh;
  uint8_t  night_end_hh;
  uint16_t night_duty;
};
struct BlobV1 {
  uint16_t magic;
  uint8_t  version;
  uint8_t  size;
  SettingsV1 s;
};

struct CalBlob {
  uint16_t magic;    // 'N'<<8|'C'
  uint16_t floor_duty;
};
constexpr uint16_t CAL_MAGIC = 0x4E43;

Settings s_settings;
NightCal s_cal = {false, 0};
bool     s_dirty = false;
uint32_t s_dirty_since_ms = 0;
constexpr uint32_t COMMIT_DEBOUNCE_MS = 2000;

Settings defaults() {
  Settings d = {};
  d.day_pct = 100;
#if defined(FEATURE_NIGHT_BLACKOUT) && FEATURE_NIGHT_BLACKOUT
  d.night_screen = NIGHT_SCREEN_OFF;
#else
  d.night_screen = NIGHT_SCREEN_DIM;
#endif
  d.red_shift = 1;
  d.peek_s = 5;
  d.night_start_hh = CD_QUIET_START_HOUR;
  d.night_end_hh = CD_QUIET_END_HOUR;
  // Uncalibrated default glow: level 4 of 10 above the fallback floor lands
  // near the bench-tuned CD_BRIGHT_NIGHT (3/255 == 96/8191) territory.
  d.night_duty = night_step_duty(NIGHT_FLOOR_DFLT, 4);
  d.character = 0;  // Character::QuietGlass
  return d;
}

void sanitize(Settings& s) {
  if (s.day_pct < 20 || s.day_pct > 100) s.day_pct = 100;
  if (s.night_screen > NIGHT_SCREEN_OFF) s.night_screen = NIGHT_SCREEN_DIM;
  if (s.red_shift > 1) s.red_shift = 1;
  if (s.peek_s != 3 && s.peek_s != 5 && s.peek_s != 10) s.peek_s = 5;
  if (s.night_start_hh > 23) s.night_start_hh = CD_QUIET_START_HOUR;
  if (s.night_end_hh > 23) s.night_end_hh = CD_QUIET_END_HOUR;
  if (s.night_duty > NIGHT_DUTY_MAX) s.night_duty = NIGHT_DUTY_MAX;
  // >= Character::Count -> default. The 4 is kept in sync by hand with
  // canary::ui::Character: glass sits BELOW ui, so no ui include here
  // (character_apply re-clamps defensively at the ui layer anyway).
  if (s.character >= 7) s.character = 0;  // = Character::Count (wave 4: 7 ages)
}

// Returns true when the blob actually landed in flash. On failure the
// dirty flag STAYS set and the caller re-arms the debounce — a broken
// storage layer must degrade to one retry per debounce window, never a
// per-loop-pass hammer (review catch: a failed begin() would have retried
// every ~5 ms pass forever).
bool commit_now() {
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/false)) return false;
  Blob b = {};
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.size = (uint8_t)sizeof(Blob);
  b.s = s_settings;
  const bool ok = p.putBytes("cfg", &b, sizeof(b)) == sizeof(b);
  p.end();
  if (ok) s_dirty = false;
  return ok;
}

}  // namespace

void settings_init() {
  s_settings = defaults();
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/true)) return;
  Blob b = {};
  BlobV1 b1 = {};
  if (p.getBytesLength("cfg") == sizeof(Blob) &&
      p.getBytes("cfg", &b, sizeof(b)) == sizeof(Blob) &&
      b.magic == BLOB_MAGIC && b.version == BLOB_VERSION &&
      b.size == sizeof(Blob)) {
    s_settings = b.s;
    sanitize(s_settings);
  } else if (p.getBytesLength("cfg") == sizeof(BlobV1) &&
             p.getBytes("cfg", &b1, sizeof(b1)) == sizeof(BlobV1) &&
             b1.magic == BLOB_MAGIC && b1.version == 1 &&
             b1.size == sizeof(BlobV1)) {
    // v1 -> v2: field-for-field; character stays the default (Quiet
    // Glass). Marked dirty so the debounced committer rewrites the blob
    // as v2 — one upgrade write, through the same retry-safe path.
    s_settings.day_pct        = b1.s.day_pct;
    s_settings.night_screen   = b1.s.night_screen;
    s_settings.red_shift      = b1.s.red_shift;
    s_settings.peek_s         = b1.s.peek_s;
    s_settings.night_start_hh = b1.s.night_start_hh;
    s_settings.night_end_hh   = b1.s.night_end_hh;
    s_settings.night_duty     = b1.s.night_duty;
    sanitize(s_settings);
    settings_mark_dirty();
    canary::log_line("GLASS",
                     "Settings upgraded from v1 - your preferences kept.");
  }
  CalBlob c = {};
  if (p.getBytesLength("ncal") == sizeof(CalBlob) &&
      p.getBytes("ncal", &c, sizeof(c)) == sizeof(CalBlob) &&
      c.magic == CAL_MAGIC && c.floor_duty >= 1 &&
      c.floor_duty <= NIGHT_FLOOR_CAP) {
    s_cal.valid = true;
    s_cal.floor_duty = c.floor_duty;
  }
  p.end();
}

const Settings& settings() { return s_settings; }
Settings& settings_mut() { return s_settings; }

void settings_mark_dirty() {
  sanitize(s_settings);
  s_dirty = true;
  s_dirty_since_ms = millis();
}

void settings_loop(uint32_t now_ms) {
  if (s_dirty && (int32_t)(now_ms - s_dirty_since_ms) >=
                     (int32_t)COMMIT_DEBOUNCE_MS) {
    if (commit_now()) {
      canary::log_line("GLASS", "Screen settings saved.");
    } else {
      // Rate-limit the retry to the debounce window (review catch).
      s_dirty_since_ms = now_ms;
      canary::log_line("GLASS",
                       "Screen settings not saved yet - retrying shortly.");
    }
  }
}

void settings_reset() {
  s_settings = defaults();
  settings_mark_dirty();
}

const NightCal& nightcal() { return s_cal; }

bool nightcal_put(uint16_t floor_duty) {
  if (floor_duty < 1) floor_duty = 1;
  if (floor_duty > NIGHT_FLOOR_CAP) return false;  // suspicious: never store
  if (s_cal.valid && s_cal.floor_duty == floor_duty) return true;  // no wear
  // RAM first ON PURPOSE: even with a broken storage layer, tonight's
  // session keeps the floor the user just spent a dark room finding. The
  // return value tells the wizard whether it survives a reboot, and the
  // wizard tells the truth on the glass (review catch: a silent RAM/flash
  // divergence would show "saved" for a floor that quietly vanishes).
  s_cal.valid = true;
  s_cal.floor_duty = floor_duty;
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/false)) return false;
  CalBlob c = {CAL_MAGIC, floor_duty};
  const bool ok = p.putBytes("ncal", &c, sizeof(c)) == sizeof(c);
  p.end();
  return ok;
}

uint8_t day_level() {
  return (uint8_t)(((uint32_t)CD_BRIGHT_DAY * s_settings.day_pct) / 100u);
}

uint8_t ambient_level() {
  return (uint8_t)(((uint32_t)CD_BRIGHT_AMBIENT * s_settings.day_pct) / 100u);
}

uint16_t night_duty_effective() {
  const uint16_t floor_d = s_cal.valid ? s_cal.floor_duty : NIGHT_FLOOR_DFLT;
  uint16_t d = s_settings.night_duty;
  if (d < floor_d) d = floor_d;
  if (d > NIGHT_DUTY_MAX) d = NIGHT_DUTY_MAX;
  return d;
}

}  // namespace canary::glass
