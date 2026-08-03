// canary/companion/night_clock.h — the Night Watch: what the glass is allowed
// to emit, minute by minute, on a device that sleeps beside someone's head.
// Pure, host-testable (no Arduino, no LVGL, no panel driver).
//
// Design: docs/design/canary_companion.md §3.
//
// ── Why this is a separate engine and not a brightness variable ──────────────
//
// The nightstand research (docs/hardware/display_nightstand.md) found the same
// complaint at the top of every review of every bedside screen ever shipped:
// *still too bright at night*. The products people actually love (Braun, the
// Loftie blackout, Mui) emit ZERO light until asked.
//
// The Canary Display could not fully take that advice. Its bench verdict was
// blunt: on a backlit LCD, "off" is not off — bleed leaves a glowing gray
// rectangle, which reads worse in a dark room than a calibrated dim glow. So
// the LCD nightstand ships a dim floor and treats blackout as a preference.
//
// **The watch board is AMOLED, and that verdict does not carry.** An unlit
// AMOLED pixel is off: no bleed, no gray, no rectangle, and no power. Black is
// free here in a way it has never been free anywhere else in this project. So
// the Night Watch inverts the display's default and goes genuinely dark — and
// this engine exists to make sure "dark" never quietly starts meaning "safe".
//
// ── The one rule that outranks the user's preference ─────────────────────────
//
// Silence is never rendered as safety. If the fleet is in a Warn-or-worse band,
// or a link is down, or the clock itself is unsure what time it is, the glass
// keeps a visible presence NO MATTER WHAT THE USER CHOSE — because a dark
// screen must always be able to be read as "all is well". A device that goes
// dark both when everything is fine and when it has lost the plot has taught
// its owner nothing, and `test_trouble_overrides_blackout` enforces it.

#ifndef CANARY_COMPANION_NIGHT_CLOCK_H
#define CANARY_COMPANION_NIGHT_CLOCK_H

#include <stdint.h>

namespace canary {
namespace companion {

// ── Brightness rungs ─────────────────────────────────────────────────────────
//
// On this panel brightness is a COMMAND, not a PWM duty (there is no backlight
// pin — see boards/waveshare-esp32s3-amoled206/pins/pins.h), so these are
// 0..255 panel levels. They are not linear in perceived brightness and are not
// meant to be: each rung is a bench-set destination, not a point on a curve.

static constexpr uint8_t NW_BRIGHT_DAY = 200;      // read at arm's length, lit room
static constexpr uint8_t NW_BRIGHT_AMBIENT = 90;   // idle, lit room
static constexpr uint8_t NW_BRIGHT_EVENING = 40;   // after quiet hours begin, pre-sleep
static constexpr uint8_t NW_BRIGHT_NIGHT = 6;      // the dimmest legible rung, 3 a.m.
static constexpr uint8_t NW_BRIGHT_PEEK = 20;      // a deliberate look, mid-night
static constexpr uint8_t NW_BRIGHT_OFF = 0;        // AMOLED: genuinely off

// ── What the glass is doing ──────────────────────────────────────────────────

enum class GlassMode : uint8_t {
  Dark = 0,   // emitting nothing. Only reachable when everything is genuinely well.
  Night,      // the dim red-shifted clock, held all night
  Peek,       // a raise or a tap, mid-night — brief and dimmer than evening
  Evening,    // quiet hours have begun but the room is still awake
  Ambient,    // daytime idle
  Awake,      // touched or raised in the daytime
  Vigil,      // trouble. Overrides every preference above. Never dark.
};

// ── How the user asked to spend the night ────────────────────────────────────
//
// Two honest choices and no third. The LCD nightstand needed a third because
// its "off" was a lie; ours does not.

enum class NightStyle : uint8_t {
  GoDark = 0,   // emit nothing; a raise or a tap peeks. The default here.
  KeepGlow,     // hold the dim clock all night — for a room that wants a marker
};

struct NightClockConfig {
  NightStyle style = NightStyle::GoDark;
  uint8_t quiet_start_hour = 22;
  uint8_t quiet_end_hour = 7;
  // How long a peek lasts before falling back. Short on purpose: long enough to
  // read a clock face twice, not long enough to light a room while you drift off.
  uint16_t peek_ms = 6000;
  // A daytime wake holds longer — you are actually using it.
  uint16_t awake_ms = 15000;
  bool wake_on_raise = true;
};

// Live inputs. `trouble` and `clock_unsure` are the two overrides; everything
// else is preference and comfort.
struct NightClockInputs {
  uint8_t hour = 12;          // local hour, 0..23
  bool trouble = false;       // fleet Warn-or-worse, or a link down
  bool clock_unsure = false;  // no RTC read and no SNTP — we do not know the time
  bool touched = false;       // a touch landed this tick
  bool raised = false;        // wake-on-raise fired this tick (raise_gesture.h)
  uint32_t now_ms = 0;
};

struct NightClockState {
  GlassMode mode = GlassMode::Ambient;
  uint32_t hold_until_ms = 0;
};

// Inclusive-start, exclusive-end, and correct across midnight (22 → 7).
inline bool nw_in_quiet_hours(const NightClockConfig& c, uint8_t hour) {
  if (c.quiet_start_hour == c.quiet_end_hour) return false;
  if (c.quiet_start_hour < c.quiet_end_hour) {
    return hour >= c.quiet_start_hour && hour < c.quiet_end_hour;
  }
  return hour >= c.quiet_start_hour || hour < c.quiet_end_hour;
}

// The deep middle of the night, where a peek must be dimmest. Distinguished
// from "quiet hours have started" because 22:30 and 03:30 are different rooms:
// at 22:30 someone is reading, at 03:30 nobody's eyes are adapted to anything.
inline bool nw_is_deep_night(const NightClockConfig& c, uint8_t hour) {
  if (!nw_in_quiet_hours(c, hour)) return false;
  // The first hour of quiet is still "evening" — people are awake in it.
  const uint8_t evening_end = static_cast<uint8_t>((c.quiet_start_hour + 1) % 24);
  if (c.quiet_start_hour < evening_end) {
    return !(hour >= c.quiet_start_hour && hour < evening_end);
  }
  return !(hour >= c.quiet_start_hour || hour < evening_end);
}

inline uint8_t nw_brightness(GlassMode m) {
  switch (m) {
    case GlassMode::Dark:    return NW_BRIGHT_OFF;
    case GlassMode::Night:   return NW_BRIGHT_NIGHT;
    case GlassMode::Peek:    return NW_BRIGHT_PEEK;
    case GlassMode::Evening: return NW_BRIGHT_EVENING;
    case GlassMode::Ambient: return NW_BRIGHT_AMBIENT;
    case GlassMode::Awake:   return NW_BRIGHT_DAY;
    case GlassMode::Vigil:   return NW_BRIGHT_EVENING;  // visible, never blinding
  }
  return NW_BRIGHT_AMBIENT;
}

// Night rendering is red-shifted: long wavelengths disturb dark adaptation and
// melatonin least. The caller maps this onto theme.h's `ncol_*` palette.
inline bool nw_red_shifted(GlassMode m) {
  return m == GlassMode::Night || m == GlassMode::Peek;
}

// One tick. Pure: same inputs, same state, same result.
inline void nw_step(NightClockState& s, const NightClockConfig& c,
                    const NightClockInputs& in) {
  const bool quiet = nw_in_quiet_hours(c, in.hour);
  const bool deep = nw_is_deep_night(c, in.hour);

  // ── The overrides, first and unconditionally ──
  //
  // Trouble, or a clock that does not know what time it is. Both mean the glass
  // must be visibly present: a dark screen is a claim of wellness, and neither
  // of these states may make that claim. Note that this runs BEFORE the hold
  // timer, so trouble arriving mid-peek does not wait for the peek to lapse.
  if (in.trouble || in.clock_unsure) {
    s.mode = GlassMode::Vigil;
    s.hold_until_ms = 0;
    return;
  }

  // ── Interaction ──
  const bool poked = in.touched || (in.raised && c.wake_on_raise);
  if (poked) {
    if (quiet) {
      s.mode = GlassMode::Peek;
      s.hold_until_ms = in.now_ms + c.peek_ms;
    } else {
      s.mode = GlassMode::Awake;
      s.hold_until_ms = in.now_ms + c.awake_ms;
    }
    return;
  }

  // ── Holding an interaction open ──
  if (s.hold_until_ms != 0) {
    // Unsigned-safe elapsed comparison: survives the millis() wrap.
    if (static_cast<uint32_t>(in.now_ms - s.hold_until_ms) >= 0x80000000u) {
      return;  // still holding
    }
    s.hold_until_ms = 0;  // lapsed; fall through to rest
  }

  // ── At rest ──
  if (!quiet) {
    s.mode = GlassMode::Ambient;
    return;
  }
  if (!deep) {
    s.mode = GlassMode::Evening;
    return;
  }
  s.mode = (c.style == NightStyle::GoDark) ? GlassMode::Dark : GlassMode::Night;
}

// ── The gentle wake ──────────────────────────────────────────────────────────
//
// Two phases, borrowed from the display's wake alarm and from every sunrise
// clock that people keep: light arrives before sound, and sound arrives quietly.
// A wrist device adds a third channel the nightstand never had — haptics — and
// it goes FIRST, because a buzz on a wrist wakes exactly one person in a bed.
//
// Nothing here escalates to a shriek. The alarm's job is to end sleep, not to
// win an argument with it.

static constexpr uint16_t WAKE_LIGHT_LEAD_MIN = 15;  // glow starts this early
static constexpr uint16_t WAKE_HAPTIC_LEAD_MIN = 2;  // wrist buzz starts this early

enum class WakePhase : uint8_t {
  Idle = 0,
  Glow,     // brightness ramps from night floor toward day over the lead
  Buzz,     // haptics join, still no sound
  Sound,    // the chime joins, from its quietest step
  Done,
};

struct WakeAlarm {
  bool enabled = false;
  uint8_t hour = 7;
  uint8_t minute = 0;
  WakePhase phase = WakePhase::Idle;
};

// Minutes until the alarm, or a large sentinel when it is not today's problem.
inline int16_t wake_minutes_until(const WakeAlarm& a, uint8_t hour, uint8_t minute) {
  if (!a.enabled) return 0x7FFF;
  const int now = hour * 60 + minute;
  const int at = a.hour * 60 + a.minute;
  int d = at - now;
  if (d < 0) d += 24 * 60;
  return static_cast<int16_t>(d);
}

inline WakePhase wake_phase_for(const WakeAlarm& a, uint8_t hour, uint8_t minute) {
  if (!a.enabled) return WakePhase::Idle;
  const int16_t d = wake_minutes_until(a, hour, minute);
  if (d == 0) return WakePhase::Sound;
  if (d <= WAKE_HAPTIC_LEAD_MIN) return WakePhase::Buzz;
  if (d <= WAKE_LIGHT_LEAD_MIN) return WakePhase::Glow;
  return WakePhase::Idle;
}

// Brightness during the glow ramp: night floor → day, linear across the lead.
// Linear in panel level rather than in perceived brightness is deliberate — the
// perceptual curve front-loads the light, which is the opposite of gentle.
inline uint8_t wake_glow_brightness(int16_t minutes_until) {
  if (minutes_until >= WAKE_LIGHT_LEAD_MIN) return NW_BRIGHT_NIGHT;
  if (minutes_until <= 0) return NW_BRIGHT_DAY;
  const int span = NW_BRIGHT_DAY - NW_BRIGHT_NIGHT;
  const int done = WAKE_LIGHT_LEAD_MIN - minutes_until;
  return static_cast<uint8_t>(NW_BRIGHT_NIGHT + (span * done) / WAKE_LIGHT_LEAD_MIN);
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_NIGHT_CLOCK_H
