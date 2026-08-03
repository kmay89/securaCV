// include/canary/care/hallway.h — Hallway mode: the nightlight, made easy.
//
// WHY THIS EXISTS
//   Everything a hallway nightlight needs was already in the tree, and none of
//   it was reachable. Getting one meant knowing that the lantern
//   (`care/lantern.h`) has an auto schedule, that the schedule is called
//   "lantern hours", that it ships off, and then separately choosing a scene,
//   a brightness and a warmth that suit a corridor at 3 a.m. That is an expert
//   path, and a nightlight is not an expert feature.
//
//   Hallway mode is ONE switch over that machinery. It does not add a second
//   way to be a lamp — it writes the lantern's own settings, so there is still
//   exactly one lamp in the firmware and exactly one place the honesty rules
//   live. Turning it off puts everything back.
//
// WHAT THE SWITCH BUYS
//   1. Lantern hours on, at a corridor-appropriate look (warm, dim, calm).
//   2. A DWELL ENVELOPE — the piece that genuinely did not exist. A hallway
//      lamp that snaps to full the instant quiet hours begin reads as a
//      timer firing; one that rises over a few minutes reads as evening
//      settling. Same at the far end: it ebbs toward sunrise instead of
//      cutting out. `level()` is that envelope.
//   3. A plumage depth (`color/plumage.h`) — how much the lamp is allowed to
//      speak. Deliberately low: a corridor light should be *noticed* when you
//      look at it, not perform at someone walking to the bathroom.
//
// THE BEACON, AND THE ONE INVARIANT THIS FEATURE TOUCHES
//   `hal/ambient_led.h` and display_nightstand_line.md §4 both say the WS2812
//   is a PURE attention channel — dark when all is well — and that the lamp
//   never routes through it. Hallway mode is the one thing that can change
//   that, so it is worth being precise about why it is allowed to.
//
//   The invariant protects one specific inference: *darkness means safe*. It
//   is already spent the moment lantern hours are on, because the glass is
//   then lit all night regardless of state — that is exactly the knowing trade
//   `lantern.h` documents. Letting the LED join a lamp that is already burning
//   costs nothing further: the room is provably calm (attention vetoes the
//   lamp before it can be lit at all), and the user has explicitly asked for a
//   light. What would be dishonest is the reverse — a beacon glowing a scene
//   color while the lamp is OFF, which could be read as "all is well". That
//   never happens: `beacon` only ever applies while the lamp is lit.
//
//   So: with Hallway mode OFF (the default) the beacon behaves exactly as it
//   always has. With it on, `beacon` lets the point of light breathe the same
//   phrase the pane is showing. A plain summoned lantern never does this —
//   that path did not make the trade.
//
//   The attention veto itself is inherited, never reimplemented: `level()`
//   takes `attention` and returns 0 for it, so a lit hallway yields to the
//   truth the instant the fleet reaches Warn.
//
// Pure logic: no Arduino, no NVS, no clock reads. The glue owns persistence
// and feeds the minute counts. Host-tested in
// tests_host/test_bedside_models.cpp.
#pragma once
#include <stdint.h>

namespace canary::care {

// How bright the corridor wants to be. Three choices, because a nightlight
// with a slider is a nightlight nobody finishes setting up.
enum HallwayLevel : uint8_t {
  HALLWAY_DIM = 0,   // barely there — find the doorframe, keep your night vision
  HALLWAY_SOFT = 1,  // the default: enough to read a step by
  HALLWAY_GLOW = 2,  // a proper little lamp, for a corridor with no other light
};

// The preset a level implies. The glue copies these onto the shared
// LookParams + the lantern's own settings; nothing here is a second source of
// truth, it is just the recommendation.
struct HallwayPreset {
  uint8_t brightness;  // LookParams::brightness
  int8_t warmth;       // LookParams::warmth — corridors want warm light
  uint8_t depth;       // plumage swell depth (0 = a plain lamp, no song)
};

class HallwayModel {
 public:
  // Rise and ebb default to a few minutes each: long enough to read as a
  // change in the light rather than a switch, short enough that someone
  // walking through at the boundary still gets a usable lamp.
  static constexpr uint16_t kRiseMinDefault = 6;
  static constexpr uint16_t kEbbMinDefault = 12;

  void configure(bool on, uint8_t level, uint16_t rise_min, uint16_t ebb_min,
                 bool beacon) {
    on_ = on;
    level_ = level > HALLWAY_GLOW ? (uint8_t)HALLWAY_SOFT : level;
    rise_min_ = rise_min ? rise_min : 1;
    ebb_min_ = ebb_min ? ebb_min : 1;
    beacon_ = beacon;
  }

  bool enabled() const { return on_; }
  uint8_t level_choice() const { return level_; }
  uint16_t rise_min() const { return rise_min_; }
  uint16_t ebb_min() const { return ebb_min_; }

  // May the WS2812 join the lamp? Only ever true while Hallway mode is on AND
  // the lamp is lit — see the note at the top. Never true for a plain
  // summoned lantern.
  bool beacon() const { return on_ && beacon_; }

  // The stored preference, independent of whether the mode is currently on.
  // The glue persists THIS, not `beacon()`: reading back the effective value
  // would silently clear the user's choice every time they switched the mode
  // off, so turning it on again would come back with a different device.
  bool beacon_pref() const { return beacon_; }

  // The look this level asks for. Warmth is positive across the board: a
  // corridor at night wants candle light, not moonlight, and the warm end of
  // the palette is also the end that disturbs dark-adapted eyes least.
  HallwayPreset preset() const {
    switch (level_) {
      case HALLWAY_DIM:  return {40, 42, 26};
      case HALLWAY_GLOW: return {150, 30, 58};
      case HALLWAY_SOFT:
      default:           return {88, 36, 40};
    }
  }

  // The dwell envelope, 0..255, to multiply the lamp's brightness by.
  //
  //   elapsed_min   minutes since quiet hours began
  //   remaining_min minutes until quiet hours end
  //   attention     (worst >= Warn) || link-down — the same flag the lantern
  //                 vetoes on. A lit hallway yields to the truth.
  //
  // Rises over rise_min, holds, then ebbs over the last ebb_min. When the two
  // ramps would overlap (a very short night, or ramps set longer than the
  // window), whichever is lower wins — so the lamp still fades in and out
  // rather than jumping, it just never reaches full.
  uint8_t level(uint16_t elapsed_min, uint16_t remaining_min,
                bool attention) const {
    if (!on_ || attention) return 0;
    const uint8_t up = ramp(elapsed_min, rise_min_);
    const uint8_t down = ramp(remaining_min, ebb_min_);
    return up < down ? up : down;
  }

  // Convenience for the glue: the brightness to hand LookParams this minute,
  // i.e. the preset scaled by the dwell. Kept here so the two are never
  // combined two different ways in two different render paths.
  uint8_t brightness_now(uint16_t elapsed_min, uint16_t remaining_min,
                         bool attention) const {
    const uint32_t b = (uint32_t)preset().brightness *
                       level(elapsed_min, remaining_min, attention) / 255u;
    return (uint8_t)b;
  }

 private:
  // Eased 0->255 across `span` minutes. Smoothstep, so the light leaves and
  // reaches its rest without a visible corner — a linear ramp is legible as a
  // ramp, which is exactly what we are trying not to look like.
  static uint8_t ramp(uint16_t mins, uint16_t span) {
    if (span == 0 || mins >= span) return 255;
    const uint32_t x = (uint32_t)mins * 255u / span;
    const uint32_t x2 = x * x / 255u;
    const uint32_t x3 = x2 * x / 255u;
    const int32_t s = (int32_t)(3u * x2) - (int32_t)(2u * x3);
    return (uint8_t)(s < 0 ? 0 : (s > 255 ? 255 : s));
  }

  bool on_ = false;
  uint8_t level_ = HALLWAY_SOFT;
  uint16_t rise_min_ = kRiseMinDefault;
  uint16_t ebb_min_ = kEbbMinDefault;
  bool beacon_ = true;
};

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
// ── Device glue (src/care/hallway.cpp) ──────────────────────────────────
// The one shared instance + NVS persistence. `hallway_set()` is the whole
// public surface of the feature: it writes the lantern's auto mode and the
// shared LookParams, so enabling Hallway mode from the settings surface, from
// MQTT, or from the BOOT button all take the identical path.
HallwayModel& hallway();
void hallway_begin();                       // NVS restore + apply
void hallway_set(bool on, uint8_t level);   // the one switch
#endif

}  // namespace canary::care
