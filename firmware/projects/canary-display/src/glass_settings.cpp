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
constexpr uint8_t  BLOB_VERSION = 5;  // v5: +clock_12h +standalone-weather
                                      // opt-in/location (v1..v4 migrate)

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

// Frozen v2 layout (pre-orientation/brightness). Same reason as v1: a v2
// blob migrates field-for-field so the 7"/dash rotation + brightness wave
// never resets the night hours, glow, or Character a user already tuned.
struct SettingsV2 {
  uint8_t  day_pct;
  uint8_t  night_screen;
  uint8_t  red_shift;
  uint8_t  peek_s;
  uint8_t  night_start_hh;
  uint8_t  night_end_hh;
  uint16_t night_duty;
  uint8_t  character;
};
struct BlobV2 {
  uint16_t magic;
  uint8_t  version;
  uint8_t  size;
  SettingsV2 s;
};

// Frozen v3 layout (pre-clock-style). Same reason as v1/v2: a v3 blob
// migrates field-for-field so the clock-face wave never resets a rotation,
// brightness, night window or Character a user already tuned.
struct SettingsV3 {
  uint8_t  day_pct;
  uint8_t  night_screen;
  uint8_t  red_shift;
  uint8_t  peek_s;
  uint8_t  night_start_hh;
  uint8_t  night_end_hh;
  uint16_t night_duty;
  uint8_t  character;
  uint8_t  rotation;
  uint8_t  bright_pct;
};
struct BlobV3 {
  uint16_t magic;
  uint8_t  version;
  uint8_t  size;
  SettingsV3 s;
};

// Frozen v4 layout (pre-12h/weather). Same reason as the others: a v4 blob
// migrates field-for-field so this wave never resets a clock face, rotation
// or night window a user already tuned.
struct SettingsV4 {
  uint8_t  day_pct;
  uint8_t  night_screen;
  uint8_t  red_shift;
  uint8_t  peek_s;
  uint8_t  night_start_hh;
  uint8_t  night_end_hh;
  uint16_t night_duty;
  uint8_t  character;
  uint8_t  rotation;
  uint8_t  bright_pct;
  uint8_t  clock_style;
};
struct BlobV4 {
  uint16_t magic;
  uint8_t  version;
  uint8_t  size;
  SettingsV4 s;
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
  // Dim, not off — on every flavor, blackout-capable or not.
  //
  // A blackout-capable glass used to DEFAULT to going fully dark at night,
  // and on a nightstand that reads as a fault rather than as a setting: the
  // clock you bought a bedside clock for is simply gone, and getting it back
  // means finding an unlit screen in the dark and remembering that a tap
  // peeks. Dim keeps the calibrated glow — the same floor the black-point
  // wizard tunes — and a tap still lifts it to CD_BRIGHT_PEEK, so "off" is
  // now something an owner chooses for a room that needs true dark rather
  // than the state they wake up to. NIGHT_SCREEN_OFF is untouched and one
  // tap away in settings; the honesty veto over it is unchanged.
  d.night_screen = NIGHT_SCREEN_DIM;
  d.red_shift = 1;
  d.peek_s = 5;
  d.night_start_hh = CD_QUIET_START_HOUR;
  d.night_end_hh = CD_QUIET_END_HOUR;
  // Uncalibrated default glow: level 4 of 10 above the fallback floor lands
  // near the bench-tuned CD_BRIGHT_NIGHT (3/255 == 96/8191) territory.
  d.night_duty = night_step_duty(NIGHT_FLOOR_DFLT, 4);
  d.character = 0;  // Character::QuietGlass
  d.rotation = ROT_LANDSCAPE;   // the native wall/desk poster
  d.bright_pct = BRIGHT_PCT_MAX;  // full glass; dim it deliberately, not by default
  d.clock_style = 0;  // ClockStyle::Segment — the classic instrument
  d.clock_12h = 1;    // 12-hour with a quiet AM/PM; 24h is one flip away
  d.wx_direct = 0;    // the hub stays the egress point unless the owner opts in
  d.wx_loc_set = 0;
  d.wx_lat10 = 0;
  d.wx_lon10 = 0;
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
  // >= Character::Count -> default. The literal is kept in sync by hand
  // with canary::ui::Character: glass sits BELOW ui, so no ui include here
  // (character_apply re-clamps defensively at the ui layer anyway).
  if (s.character >= 7) s.character = 0;  // = Character::Count (wave 4: 7 ages)
  // Same hand-synced literal discipline as `character`: 5 = ClockStyle::Count
  // (clock_styles.h). Anything past the ring degrades to Segment.
  if (s.clock_style >= 5) s.clock_style = 0;
  if (s.clock_12h > 1) s.clock_12h = 1;
  if (s.wx_direct > 1) s.wx_direct = 0;
  if (s.wx_loc_set > 1) s.wx_loc_set = 0;
  if (s.wx_lat10 < -900 || s.wx_lat10 > 900 ||
      s.wx_lon10 < -1800 || s.wx_lon10 > 1800) {
    // A corrupt coordinate must not aim the fetcher at the wrong sky: the
    // location degrades to unset, and the opt-in simply waits for a new one.
    s.wx_loc_set = 0;
    s.wx_lat10 = 0;
    s.wx_lon10 = 0;
  }
  s.rotation &= 3;  // 0..3 = 0/90/180/270; any other bit pattern is noise
  // bright_pct lives on a [50..100] grid; a value off the grid (or the 0 a
  // migrated blob leaves) snaps to full rather than a random dim.
  if (s.bright_pct < BRIGHT_PCT_MIN || s.bright_pct > BRIGHT_PCT_MAX)
    s.bright_pct = BRIGHT_PCT_MAX;
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
  BlobV2 b2 = {};
  BlobV3 b3 = {};
  BlobV4 b4 = {};
  if (p.getBytesLength("cfg") == sizeof(Blob) &&
      p.getBytes("cfg", &b, sizeof(b)) == sizeof(Blob) &&
      b.magic == BLOB_MAGIC && b.version == BLOB_VERSION &&
      b.size == sizeof(Blob)) {
    s_settings = b.s;
    sanitize(s_settings);
  } else if (p.getBytesLength("cfg") == sizeof(BlobV4) &&
             p.getBytes("cfg", &b4, sizeof(b4)) == sizeof(BlobV4) &&
             b4.magic == BLOB_MAGIC && b4.version == 4 &&
             b4.size == sizeof(BlobV4)) {
    // v4 -> v5: field-for-field; the clock gains its quiet AM/PM (the new
    // default) and the weather opt-in stays off. Marked dirty so the
    // debounced committer rewrites the blob as v5.
    s_settings.day_pct        = b4.s.day_pct;
    s_settings.night_screen   = b4.s.night_screen;
    s_settings.red_shift      = b4.s.red_shift;
    s_settings.peek_s         = b4.s.peek_s;
    s_settings.night_start_hh = b4.s.night_start_hh;
    s_settings.night_end_hh   = b4.s.night_end_hh;
    s_settings.night_duty     = b4.s.night_duty;
    s_settings.character      = b4.s.character;
    s_settings.rotation       = b4.s.rotation;
    s_settings.bright_pct     = b4.s.bright_pct;
    s_settings.clock_style    = b4.s.clock_style;
    s_settings.clock_12h      = 1;
    sanitize(s_settings);
    settings_mark_dirty();
    canary::log_line("GLASS",
                     "Settings upgraded from v4 - your preferences kept.");
  } else if (p.getBytesLength("cfg") == sizeof(BlobV3) &&
             p.getBytes("cfg", &b3, sizeof(b3)) == sizeof(BlobV3) &&
             b3.magic == BLOB_MAGIC && b3.version == 3 &&
             b3.size == sizeof(BlobV3)) {
    // v3 -> v4: field-for-field; the clock face stays Segment (the
    // zero-initialized new field IS the default). Marked dirty so the
    // debounced committer rewrites the blob as v4.
    s_settings.day_pct        = b3.s.day_pct;
    s_settings.night_screen   = b3.s.night_screen;
    s_settings.red_shift      = b3.s.red_shift;
    s_settings.peek_s         = b3.s.peek_s;
    s_settings.night_start_hh = b3.s.night_start_hh;
    s_settings.night_end_hh   = b3.s.night_end_hh;
    s_settings.night_duty     = b3.s.night_duty;
    s_settings.character      = b3.s.character;
    s_settings.rotation       = b3.s.rotation;
    s_settings.bright_pct     = b3.s.bright_pct;
    s_settings.clock_style    = 0;
    s_settings.clock_12h      = 1;
    sanitize(s_settings);
    settings_mark_dirty();
    canary::log_line("GLASS",
                     "Settings upgraded from v3 - your preferences kept.");
  } else if (p.getBytesLength("cfg") == sizeof(BlobV2) &&
             p.getBytes("cfg", &b2, sizeof(b2)) == sizeof(BlobV2) &&
             b2.magic == BLOB_MAGIC && b2.version == 2 &&
             b2.size == sizeof(BlobV2)) {
    // v2 -> v4: field-for-field; rotation stays landscape, brightness stays
    // full and the clock face stays Segment (the sanitize below snaps the
    // zero-initialized new fields to their defaults). Marked dirty so the
    // committer rewrites it as v4.
    s_settings.day_pct        = b2.s.day_pct;
    s_settings.night_screen   = b2.s.night_screen;
    s_settings.red_shift      = b2.s.red_shift;
    s_settings.peek_s         = b2.s.peek_s;
    s_settings.night_start_hh = b2.s.night_start_hh;
    s_settings.night_end_hh   = b2.s.night_end_hh;
    s_settings.night_duty     = b2.s.night_duty;
    s_settings.character      = b2.s.character;
    s_settings.clock_12h      = 1;
    sanitize(s_settings);
    settings_mark_dirty();
    canary::log_line("GLASS",
                     "Settings upgraded from v2 - your preferences kept.");
  } else if (p.getBytesLength("cfg") == sizeof(BlobV1) &&
             p.getBytes("cfg", &b1, sizeof(b1)) == sizeof(BlobV1) &&
             b1.magic == BLOB_MAGIC && b1.version == 1 &&
             b1.size == sizeof(BlobV1)) {
    // v1 -> v4: field-for-field; character, rotation, brightness and clock
    // face stay at their defaults. Marked dirty so the debounced committer
    // rewrites the blob as v4 — one upgrade write, through the same
    // retry-safe path.
    s_settings.day_pct        = b1.s.day_pct;
    s_settings.night_screen   = b1.s.night_screen;
    s_settings.red_shift      = b1.s.red_shift;
    s_settings.peek_s         = b1.s.peek_s;
    s_settings.night_start_hh = b1.s.night_start_hh;
    s_settings.night_end_hh   = b1.s.night_end_hh;
    s_settings.night_duty     = b1.s.night_duty;
    s_settings.clock_12h      = 1;
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
