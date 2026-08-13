// include/canary/ui/clock_styles.h — the curated clock-face ring for the
// drawn-clock glass (the 7" bedside/wall faces and the portrait column).
//
// Same design law as the Character ring (display_character.md §4, the
// watch-face lessons): a few NAMED, pre-validated looks a person flips
// through — never a parts bin of stroke sliders. A style changes how the
// time is DRAWN, nothing about what the glass says: the honesty lines, the
// severity colors and the night mode are untouched, and night still
// outranks everything (a red-shifted Analog is red-shifted the same way a
// red-shifted Segment is).
//
// Pure C++ on purpose (no LVGL, no Arduino): the ring is served over
// /api/settings by name so the phone renders the device's own list, and the
// geometry knobs are host-testable.
#pragma once
#include <stdint.h>

namespace canary::ui {

// Segment MUST stay 0: it is the default and the safe fallback — an old or
// corrupt settings blob degrades to it by construction (the same rule
// Character::QuietGlass keeps).
enum class ClockStyle : uint8_t {
  Segment  = 0,   // the classic seven-segment instrument (the original face)
  Slab     = 1,   // the same digits, heavyset — read from across the room
  Hairline = 2,   // thin strokes, no resting ghost — the minimal face
  Analog   = 3,   // a drawn dial: ring, twelve marks, two hands
  Calendar = 4,   // slimmer digits + the month at a glance (day face only;
                  // night keeps the room's one instrument, the clock)
  Count    = 5,
};

inline uint8_t clock_style_count() { return (uint8_t)ClockStyle::Count; }

inline const char* clock_style_name(uint8_t s) {
  switch ((ClockStyle)s) {
    case ClockStyle::Slab:     return "Slab";
    case ClockStyle::Hairline: return "Hairline";
    case ClockStyle::Analog:   return "Analog";
    case ClockStyle::Calendar: return "Calendar";
    default:                   return "Segment";
  }
}

inline const char* clock_style_caption(uint8_t s) {
  switch ((ClockStyle)s) {
    case ClockStyle::Slab:     return "heavyset \xE2\x80\xA2 reads across the room";
    case ClockStyle::Hairline: return "thin \xE2\x80\xA2 quiet \xE2\x80\xA2 no scaffolding";
    case ClockStyle::Analog:   return "a dial \xE2\x80\xA2 twelve marks \xE2\x80\xA2 two hands";
    case ClockStyle::Calendar: return "the month at a glance \xE2\x80\xA2 by day";
    default:                   return "the classic instrument";
  }
}

// The segment-family geometry knobs. Thickness scales the face's own base
// stroke (so day and night keep their proportions); ghost_day says whether
// unlit segments keep the instrument's resting shape by day. Night never
// shows ghosts regardless — a dark room wants digits, not scaffolding.
struct SegStyle {
  uint8_t t_pct;     // stroke thickness, percent of the face's base
  bool ghost_day;    // unlit segments keep a faint presence by day
};

inline SegStyle seg_style(uint8_t s) {
  switch ((ClockStyle)s) {
    case ClockStyle::Slab:     return {150, true};
    case ClockStyle::Hairline: return {55, false};
    case ClockStyle::Calendar: return {84, true};    // slimmer, to share the hero
    default:                   return {100, true};   // Segment (and Analog's fallback)
  }
}

inline bool clock_style_is_analog(uint8_t s) {
  return (ClockStyle)s == ClockStyle::Analog;
}

inline bool clock_style_is_calendar(uint8_t s) {
  return (ClockStyle)s == ClockStyle::Calendar;
}

// The one 12-hour conversion, shared by every drawn face so a digit and its
// AM/PM word can never disagree. Returns the hour to DRAW; *pm is the quiet
// marker's truth. 0 -> 12 AM, 12 -> 12 PM (the two everyone gets wrong).
inline int clock_display_hour(int hh, bool twelve, bool* pm) {
  if (pm) *pm = hh >= 12;
  if (!twelve) return hh;
  const int h = hh % 12;
  return h == 0 ? 12 : h;
}

// The flip-through ring: display order IS enum order here (unlike the
// Character ring there is no curated re-ordering), stepping wraps.
inline uint8_t clock_style_step(uint8_t s, int dir) {
  const int n = (int)ClockStyle::Count;
  int v = ((int)(s % n) + dir) % n;
  if (v < 0) v += n;
  return (uint8_t)v;
}

}  // namespace canary::ui
