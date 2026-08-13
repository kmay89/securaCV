// include/canary/glass_settings.h — runtime screen settings ("Quiet Glass"
// settings wave, docs/hardware/display_settings.md).
//
// One versioned blob holds the user's preferences; the per-panel black-point
// calibration lives under its OWN storage key so "reset settings" never
// wipes a floor the user spent a dark room finding (and re-running the
// wizard never touches the preferences).
//
// Writes are debounced: editors mutate RAM and call settings_mark_dirty();
// settings_loop() commits ~2 s after the last change, so a slider drag costs
// one flash write, not hundreds.
#pragma once
#include <stdint.h>

namespace canary::glass {

// "Screen at night" — one decision, not a pile of toggles. Dim keeps the
// calibrated glow; Off darkens the glass and a tap peeks the clock. The
// honesty veto in main.cpp still outranks Off: a dead link or a Warn+
// condition keeps the glow regardless (silence is never rendered as safety).
enum NightScreen : uint8_t {
  NIGHT_SCREEN_DIM = 0,
  NIGHT_SCREEN_OFF = 1,
};

// Display orientation — the 7"/dash RGB glass only (the round watch and the
// fixed-portrait SPI nightstands ignore it). 0..3 = 0°/90°/180°/270°
// clockwise; the odd values are portrait, the 800x480 landscape glass turned
// on its side for a wall column or a tall bedside face. Rotation is rendered
// in software (LVGL) — the panel always scans its native landscape.
enum Rotation : uint8_t {
  ROT_LANDSCAPE     = 0,   // 0°   — the native wall/desk poster (default)
  ROT_PORTRAIT      = 1,   // 90°  — turned clockwise; the column face
  ROT_LANDSCAPE_INV = 2,   // 180° — upside down (ceiling / cable-up mounts)
  ROT_PORTRAIT_INV  = 3,   // 270° — turned counter-clockwise
};

// "Up to 50% sustained": the 7"/dash backlight is binary (CH422G, no PWM),
// so daytime brightness is a RENDERED luminance scrim, not a driven level.
// The slider dims the always-on glass down to half and no darker — going
// darker than this is Night's job, not a brightness a wall panel should sit
// at all day (and a rendered scrim can't lower real backlight power).
constexpr uint8_t BRIGHT_PCT_MIN  = 50;
constexpr uint8_t BRIGHT_PCT_MAX  = 100;
constexpr uint8_t BRIGHT_PCT_STEP = 10;

struct Settings {
  uint8_t  day_pct;        // 20..100 — scales day + ambient brightness
  uint8_t  night_screen;   // NightScreen
  uint8_t  red_shift;      // 1 = red-shifted night palette (default)
  uint8_t  peek_s;         // 3 / 5 / 10 — dark-hours tap-to-peek window
  uint8_t  night_start_hh; // 0..23 local
  uint8_t  night_end_hh;   // 0..23 local
  uint16_t night_duty;     // night-profile duty (13-bit), floor-clamped
  uint8_t  character;      // canary::ui::Character; 0 = Quiet Glass (default)
  uint8_t  rotation;       // Rotation; 0 = landscape (default). Dash glass only.
  uint8_t  bright_pct;     // 50..100 rendered daytime luminance. Dash glass only.
  uint8_t  clock_style;    // canary::ui::ClockStyle; 0 = Segment (default).
                           // Drawn-clock faces only (7"/portrait column).
  uint8_t  clock_12h;      // 1 = 12-hour digits with a quiet AM/PM (default);
                           // 0 = 24-hour. Drawn-clock faces only.
  // Standalone weather (FEATURE_STANDALONE_WEATHER, hub-less homes only):
  // the OPT-IN for the device to fetch its own forecast — an anonymous,
  // coarse-location query, and only while no hub was ever configured
  // (mqtt_broker_is_placeholder). Never a fallback when a hub exists.
  uint8_t  wx_direct;      // 0 = off (default; the hub is the egress point)
  uint8_t  wx_loc_set;     // 1 once a coarse location has been stored
  int16_t  wx_lat10;       // latitude  x10 (-900..900) — ~11 km grid, by design
  int16_t  wx_lon10;       // longitude x10 (-1800..1800)
};

struct NightCal {
  bool     valid;
  uint16_t floor_duty;     // first night-profile duty this panel emits at
};

// The night backlight profile: 1 kHz / 13-bit gives ~30 distinguishable
// steps inside what the 8-bit day profile calls "duty 1" — that resolution
// is what makes a per-panel calibrated floor possible at all. Floors past
// ~5% mean something is wrong (inverted polarity, wrong pin), so the wizard
// warns instead of storing.
constexpr uint16_t NIGHT_DUTY_MAX   = 8191;
constexpr uint16_t NIGHT_FLOOR_CAP  = 410;   // ~5% — beyond this: suspicious
constexpr uint16_t NIGHT_FLOOR_DFLT = 64;    // uncalibrated fallback floor
constexpr int      NIGHT_STEPS      = 10;    // user-facing "level 1..10"

void settings_init();
const Settings& settings();
Settings& settings_mut();          // mutate, then settings_mark_dirty()
void settings_mark_dirty();
void settings_loop(uint32_t now_ms);
void settings_reset();             // defaults; calibration untouched

const NightCal& nightcal();
// Store the calibrated floor. RAM updates either way (tonight works even
// with a broken storage layer); the return says whether it survives a
// reboot — false = refused (suspicious floor) or flash write failed, and
// the wizard tells the truth on the glass.
bool nightcal_put(uint16_t floor_duty);

// Derived day-profile levels (0..255), scaled by day_pct.
uint8_t day_level();
uint8_t ambient_level();

// Effective night glow duty: the stored preference clamped to the panel's
// calibrated floor (or the conservative default floor when uncalibrated).
uint16_t night_duty_effective();

// ── Pure helpers (host-testable; no storage, no Arduino) ─────────────────

// Level 1..NIGHT_STEPS -> duty on a log curve from floor to ~10x floor.
// Log, not linear: at the bottom of a backlight the eye works in ratios.
inline uint16_t night_step_duty(uint16_t floor_duty, int step) {
  if (floor_duty < 1) floor_duty = 1;
  if (step < 1) step = 1;
  if (step > NIGHT_STEPS) step = NIGHT_STEPS;
  // duty = floor * 10^((step-1)/9), integerized: multiply by 1.2915^step-1
  // via a small table to keep it exact and monotone on-device and in tests.
  static const uint16_t kNum[NIGHT_STEPS] = {
      1000, 1292, 1668, 2154, 2783, 3594, 4642, 5995, 7743, 10000};
  uint32_t d = ((uint32_t)floor_duty * kNum[step - 1]) / 1000u;
  if (d > NIGHT_DUTY_MAX) d = NIGHT_DUTY_MAX;
  if (d < floor_duty) d = floor_duty;
  return (uint16_t)d;
}

// Nearest step for a stored duty (inverse of night_step_duty).
inline int night_duty_step(uint16_t floor_duty, uint16_t duty) {
  int best = 1;
  uint32_t best_err = UINT32_MAX;
  for (int i = 1; i <= NIGHT_STEPS; i++) {
    const uint16_t d = night_step_duty(floor_duty, i);
    const uint32_t err = d > duty ? (uint32_t)(d - duty) : (uint32_t)(duty - d);
    if (err < best_err) { best_err = err; best = i; }
  }
  return best;
}

// Quiet-hours window test, midnight-wrap aware. start==end = never night.
inline bool hours_in_window(int hh, int start_hh, int end_hh) {
  if (start_hh == end_hh) return false;
  if (start_hh < end_hh) return hh >= start_hh && hh < end_hh;
  return hh >= start_hh || hh < end_hh;
}

// WHERE in the quiet-hours window we are, in minutes: how long it has been
// running and how long is left. Midnight-wrap aware, like the test above.
//
// Hallway mode (care/hallway.h) needs this and the bool is not enough: a
// corridor light that rises over the first minutes of the window and ebbs over
// the last reads as evening settling, where one that snaps on the instant the
// hour ticks reads as a timer firing. Both outputs are 0 outside the window,
// which is also the "no schedule known" answer the caller treats as "skip the
// dwell" rather than guessing.
inline void hours_window_position(int hh, int mm, int start_hh, int end_hh,
                                  uint16_t* elapsed_min,
                                  uint16_t* remaining_min) {
  if (elapsed_min) *elapsed_min = 0;
  if (remaining_min) *remaining_min = 0;
  if (!hours_in_window(hh, start_hh, end_hh)) return;

  const int span_h = start_hh < end_hh ? (end_hh - start_hh)
                                       : (24 - start_hh + end_hh);
  const int into = ((hh - start_hh) + 24) % 24;
  const int elapsed = into * 60 + mm;
  const int span = span_h * 60;
  if (elapsed_min) *elapsed_min = (uint16_t)(elapsed < 0 ? 0 : elapsed);
  if (remaining_min)
    *remaining_min = (uint16_t)(span > elapsed ? span - elapsed : 0);
}

// ── Orientation geometry (host-testable; no LVGL, no Arduino) ────────────

inline bool rotation_is_portrait(uint8_t rot) { return (rot & 1) != 0; }

inline const char* rotation_name(uint8_t rot) {
  switch (rot & 3) {
    case ROT_PORTRAIT:      return "portrait";
    case ROT_LANDSCAPE_INV: return "landscape flipped";
    case ROT_PORTRAIT_INV:  return "portrait flipped";
    default:                return "landscape";
  }
}

// The logical LVGL canvas for a rotation, given the panel's native landscape
// size. Portrait swaps the axes; the UI lays itself out in these dimensions.
inline void rotation_logical_dims(uint8_t rot, int native_w, int native_h,
                                  int* out_w, int* out_h) {
  if (rotation_is_portrait(rot)) { *out_w = native_h; *out_h = native_w; }
  else                          { *out_w = native_w; *out_h = native_h; }
}

// Map a raw native-panel touch (0..native_w-1, 0..native_h-1) into the
// rotated logical frame the UI drew itself in — the exact inverse of the
// software render rotation, so a tap lands where the finger points at any
// orientation. (Bench note: if an axis reads mirrored on real glass, flip
// the sign on that branch — the panel's touch origin is validated there.)
inline void rotation_map_touch(uint8_t rot, int native_w, int native_h,
                               int raw_x, int raw_y, int* out_x, int* out_y) {
  switch (rot & 3) {
    case ROT_PORTRAIT:  // 90° CW: (px,py) -> (py, native_w-1-px)
      *out_x = raw_y;
      *out_y = native_w - 1 - raw_x;
      break;
    case ROT_LANDSCAPE_INV:  // 180°
      *out_x = native_w - 1 - raw_x;
      *out_y = native_h - 1 - raw_y;
      break;
    case ROT_PORTRAIT_INV:  // 270° CW: (px,py) -> (native_h-1-py, px)
      *out_x = native_h - 1 - raw_y;
      *out_y = raw_x;
      break;
    default:  // ROT_LANDSCAPE
      *out_x = raw_x;
      *out_y = raw_y;
      break;
  }
}

// ── Standalone-weather location (host-testable) ──────────────────────────

// The app sends the coarse location as ONE integer so the store can never
// hold half a coordinate: v = (lat10 + 900) * 4000 + (lon10 + 1800).
// Decode returns false (and stores nothing) outside the valid grid.
constexpr long WX_LOC_MAX = 1800L * 4000L + 3600L;
inline long wx_loc_encode(int lat10, int lon10) {
  return (long)(lat10 + 900) * 4000L + (long)(lon10 + 1800);
}
inline bool wx_loc_decode(long v, int16_t* lat10, int16_t* lon10) {
  if (v < 0 || v > WX_LOC_MAX) return false;
  const long la = v / 4000L - 900L;
  const long lo = v % 4000L - 1800L;
  if (la < -900 || la > 900 || lo < -1800 || lo > 1800) return false;
  *lat10 = (int16_t)la;
  *lon10 = (int16_t)lo;
  return true;
}

// ── Rendered brightness (host-testable) ──────────────────────────────────

// Clamp a brightness percent to the [50..100] sustained range.
inline uint8_t bright_pct_clamp(int pct) {
  if (pct < BRIGHT_PCT_MIN) return BRIGHT_PCT_MIN;
  if (pct > BRIGHT_PCT_MAX) return BRIGHT_PCT_MAX;
  return (uint8_t)pct;
}

// Black-overlay opacity (0..255) for a brightness percent: 100% = clear,
// 50% floor = a half-strength scrim (LVGL LV_OPA_50 territory). This is the
// sustained daytime dimming the binary backlight can't do in hardware.
inline uint8_t bright_scrim_opa(uint8_t bright_pct) {
  if (bright_pct >= BRIGHT_PCT_MAX) return 0;
  const uint8_t p = bright_pct_clamp(bright_pct);
  return (uint8_t)(((100 - (int)p) * 255) / 100);
}

}  // namespace canary::glass
