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
constexpr uint8_t  BLOB_VERSION = 1;

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
}

void commit_now() {
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/false)) return;
  Blob b = {};
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.size = (uint8_t)sizeof(Blob);
  b.s = s_settings;
  p.putBytes("cfg", &b, sizeof(b));
  p.end();
  s_dirty = false;
}

}  // namespace

void settings_init() {
  s_settings = defaults();
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/true)) return;
  Blob b = {};
  if (p.getBytesLength("cfg") == sizeof(Blob) &&
      p.getBytes("cfg", &b, sizeof(b)) == sizeof(Blob) &&
      b.magic == BLOB_MAGIC && b.version == BLOB_VERSION &&
      b.size == sizeof(Blob)) {
    s_settings = b.s;
    sanitize(s_settings);
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
    commit_now();
    canary::log_line("GLASS", "Screen settings saved.");
  }
}

void settings_reset() {
  s_settings = defaults();
  settings_mark_dirty();
}

const NightCal& nightcal() { return s_cal; }

void nightcal_put(uint16_t floor_duty) {
  if (floor_duty < 1) floor_duty = 1;
  if (floor_duty > NIGHT_FLOOR_CAP) return;  // suspicious floor: never store
  s_cal.valid = true;
  s_cal.floor_duty = floor_duty;
  Preferences p;
  if (!p.begin(STORE_NS, /*readOnly=*/false)) return;
  CalBlob c = {CAL_MAGIC, floor_duty};
  p.putBytes("ncal", &c, sizeof(c));
  p.end();
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
