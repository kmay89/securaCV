// doorbell_logic.h — the Canary Vision Doorbell's button and glow ring, as
// pure logic (no Arduino, no ESP-IDF): host-tested by
// firmware/tests_host/test_doorbell_logic.cpp, driven on the device by
// firmware/projects/canary-vision/src/doorbell.cpp.
//
// Two small machines:
//
//  1. THE BUTTON (sample()). Raw pin levels (or the exact edges an ISR
//     timestamped) go in; at most one event per call comes out. A press
//     rings once, however long it is held. A second press inside the
//     holdoff is a REPEAT — counted, never sealed — so a visitor jabbing
//     the button cannot flood the signed record. A button held past
//     stuck_ms (ice, tape, a jammed plunger) is a fault, reported once,
//     and it rings nothing until it has been released. A button already
//     down at boot is armed only after it is released: power-on never
//     rings the house.
//
//  2. THE GLOW (glow_target() / glow_step()). The ring is honest to the
//     visitor (docs/hardware/canary_doorbell_research.md §4.4):
//       AWAKE   breathing — the witness is on, nothing is recorded;
//       UNSURE  a slower, shallower breath — the witness has lost its
//               hub, so an empty log reads as "unsure", never as "quiet";
//       FAULT   steady and low — the button is stuck;
//       OFF     dark — the doorbell is switched off.
//     A sealed ring adds a SWELL on top: up to full, a short hold, a
//     long ease back down — "the house heard you". glow_step() slews the
//     output at a bounded rate, so no mode change, swell or setting can
//     ever make the ring flash: that is a property of this file, held by
//     the host test, not a promise in a comment.

#pragma once

#include <stdint.h>

namespace doorbell {

// ---------------------------------------------------------------------------
// The button
// ---------------------------------------------------------------------------

struct ButtonPolicy {
  uint32_t debounce_ms;  // a level must hold this long to count
  uint32_t holdoff_ms;   // a press this soon after a ring is a REPEAT
  uint32_t stuck_ms;     // held this long = a stuck button (fault)
};

// 30 ms rejects contact bounce on a 12–16 mm panel switch and still takes
// the shortest deliberate tap; 3 s is long enough that one impatient
// visitor seals one ring, short enough that a second visit rings again;
// 15 s is far past any press a person makes.
constexpr ButtonPolicy kDefaultPolicy{30, 3000, 15000};

enum class ButtonEvent : uint8_t {
  NONE = 0,
  RING,          // a press that rings: seal it, tell the house, swell
  REPEAT,        // a press inside the holdoff: counted, not sealed
  STUCK,         // held past stuck_ms: report the fault once
  STUCK_CLEARED, // the stuck button was released
};

struct ButtonState {
  bool     raw;           // last level seen (true = pressed)
  uint32_t raw_since;     // when `raw` last changed
  bool     stable;        // debounced level
  uint32_t pressed_at;    // when `stable` last went pressed
  uint32_t last_ring_ms;  // when the last RING fired
  bool     rang;          // a RING has fired since boot
  bool     armed;         // false until a released level has been seen
  bool     stuck;         // STUCK fired and the button is still down
  uint32_t repeats;       // REPEAT count since boot (diagnostics)
};

// Start from the level the pin shows at boot. A pressed level leaves the
// button disarmed until it has been released, so a button jammed at
// power-on (or wired to a closed contact) never rings.
inline ButtonState begin(bool pressed_now, uint32_t now_ms) {
  ButtonState s{};
  s.raw = pressed_now;
  s.raw_since = now_ms;
  s.stable = pressed_now;
  s.pressed_at = now_ms;
  s.armed = !pressed_now;
  return s;
}

// Feed one observation: the level `pressed` as of `now_ms`. Times may come
// from an ISR's edge stamps as long as they are fed in order; all
// arithmetic is unsigned subtraction, so a millis() wrap is harmless.
inline ButtonEvent sample(ButtonState* s, bool pressed, uint32_t now_ms,
                          const ButtonPolicy& p = kDefaultPolicy) {
  ButtonEvent ev = ButtonEvent::NONE;

  // 1. Settle: has the pending raw level held long enough to count?
  if (s->raw != s->stable && (uint32_t)(now_ms - s->raw_since) >= p.debounce_ms) {
    s->stable = s->raw;
    const uint32_t edge_ms = s->raw_since;  // the level became true here
    if (s->stable) {
      s->pressed_at = edge_ms;
      if (!s->armed) {
        // still waiting for the first release: nothing rings
      } else if (!s->rang || (uint32_t)(edge_ms - s->last_ring_ms) >= p.holdoff_ms) {
        s->rang = true;
        s->last_ring_ms = edge_ms;
        ev = ButtonEvent::RING;
      } else {
        ++s->repeats;
        ev = ButtonEvent::REPEAT;
      }
    } else {
      s->armed = true;
      if (s->stuck) {
        s->stuck = false;
        ev = ButtonEvent::STUCK_CLEARED;
      }
    }
  }

  // 2. Stuck: a debounced press held past the limit (one event per hold).
  if (ev == ButtonEvent::NONE && s->stable && s->armed && !s->stuck &&
      (uint32_t)(now_ms - s->pressed_at) >= p.stuck_ms) {
    s->stuck = true;
    ev = ButtonEvent::STUCK;
  }

  // 3. Record the new raw level for the next settle.
  if (pressed != s->raw) {
    s->raw = pressed;
    s->raw_since = now_ms;
  }
  return ev;
}

// ---------------------------------------------------------------------------
// The glow ring
// ---------------------------------------------------------------------------

constexpr uint16_t kDutyMax = 1023;  // 10-bit LEDC

enum class GlowMode : uint8_t { OFF = 0, AWAKE, UNSURE, FAULT };

// The brightness setting's floor: a doorbell that is ON keeps a visible
// glow, because a dark ring must mean a dark doorbell (§4.4). Switching
// the doorbell off is the only way to darken it.
constexpr uint8_t kGlowPctMin = 10;
constexpr uint8_t kGlowPctMax = 100;
constexpr uint8_t kGlowPctDefault = 60;

inline uint8_t clamp_glow_pct(long v) {
  if (v < kGlowPctMin) return kGlowPctMin;
  if (v > kGlowPctMax) return kGlowPctMax;
  return (uint8_t)v;
}

struct Breath {
  uint32_t period_ms;
  uint16_t lo_permille;  // of the brightness setting
  uint16_t hi_permille;
};
constexpr Breath kAwakeBreath{6000, 150, 450};    // calm, clearly alive
constexpr Breath kUnsureBreath{12000, 80, 220};   // slower and shallower
constexpr uint16_t kFaultPermille = 120;          // steady, low

// The swell after a sealed ring: rise, hold at full, ease down.
constexpr uint32_t kSwellRiseMs = 350;
constexpr uint32_t kSwellHoldMs = 900;
constexpr uint32_t kSwellFallMs = 2200;
constexpr uint32_t kSwellTotalMs = kSwellRiseMs + kSwellHoldMs + kSwellFallMs;

// glow_step()'s slew limit: the most the duty may move per millisecond.
// 4/ms crosses full scale in ~256 ms — fast enough for the swell to land
// while the visitor is still looking, slow enough that no transition is
// ever a flash.
constexpr uint32_t kSlewPerMs = 4;

struct GlowState {
  GlowMode mode;
  uint8_t  pct;            // brightness setting, kGlowPctMin..kGlowPctMax
  bool     swelling;
  uint32_t swell_at;       // when the current swell started
  uint16_t duty;           // last output (glow_step's slew memory)
  uint32_t last_step_ms;
};

inline GlowState glow_begin(uint8_t pct, uint32_t now_ms) {
  GlowState g{};
  g.mode = GlowMode::OFF;
  g.pct = clamp_glow_pct(pct);
  g.last_step_ms = now_ms;
  return g;
}

inline void glow_swell(GlowState* g, uint32_t now_ms) {
  g->swelling = true;
  g->swell_at = now_ms;
}

// A smooth 0..1000..0 wave over `period`: a cosine from a small table,
// linearly interpolated (no libm, so the host and the device agree to the
// unit). phase 0 is the trough.
inline uint16_t wave_permille(uint32_t t_ms, uint32_t period_ms) {
  // (1 - cos(2πx)) / 2 sampled at x = i/32, i = 0..16, in permille
  static const uint16_t kHalf[17] = {
      0, 10, 38, 84, 146, 222, 309, 402, 500, 598, 691, 778, 854, 916, 962, 990, 1000};
  if (period_ms == 0) return 0;
  const uint32_t ph = (uint32_t)(((uint64_t)(t_ms % period_ms) * 1024u) / period_ms);  // 0..1023
  const uint32_t half = ph <= 512 ? ph : 1024 - ph;                                // 0..512
  const uint32_t idx = half >> 5;                                                  // 0..16
  if (idx >= 16) return 1000;                                                      // the crest
  const uint32_t frac = half & 31;
  return (uint16_t)(kHalf[idx] + ((kHalf[idx + 1] - kHalf[idx]) * frac) / 32);
}

// The least duty a lit ring is ever given. Squaring a 10 % setting's
// breath trough rounds to zero, which would turn "on" into "dark" — the
// one thing the ring must never say (§4.4).
constexpr uint16_t kDutyFloor = 12;

// Perceptual brightness (permille of the setting) to LEDC duty: squared,
// close to the eye's gamma, so the breath reads as even rather than as a
// long bright plateau — riding on kDutyFloor whenever the ring is lit.
inline uint16_t permille_to_duty(uint32_t permille, uint8_t pct) {
  if (permille > 1000) permille = 1000;
  const uint32_t lin = permille * pct / 100;  // 0..1000
  if (lin == 0) return 0;
  return (uint16_t)(kDutyFloor + (lin * lin * (kDutyMax - kDutyFloor)) / 1000000u);
}

// Where the ring wants to be right now, before slewing.
inline uint16_t glow_target(GlowState* g, uint32_t now_ms) {
  uint32_t base = 0;  // permille
  switch (g->mode) {
    case GlowMode::OFF:
      g->swelling = false;
      return 0;
    case GlowMode::AWAKE:
    case GlowMode::UNSURE: {
      const Breath& b = g->mode == GlowMode::AWAKE ? kAwakeBreath : kUnsureBreath;
      base = b.lo_permille + ((uint32_t)(b.hi_permille - b.lo_permille) *
                              wave_permille(now_ms, b.period_ms)) / 1000;
      break;
    }
    case GlowMode::FAULT:
      base = kFaultPermille;
      break;
  }
  uint32_t level = base;
  if (g->swelling) {
    const uint32_t t = now_ms - g->swell_at;
    uint32_t s = 0;  // the swell's own permille
    if (t >= kSwellTotalMs) {
      g->swelling = false;
    } else if (t < kSwellRiseMs) {
      s = (t * 1000) / kSwellRiseMs;
    } else if (t < kSwellRiseMs + kSwellHoldMs) {
      s = 1000;
    } else {
      const uint32_t f = t - kSwellRiseMs - kSwellHoldMs;
      s = 1000 - (f * 1000) / kSwellFallMs;
    }
    if (s > level) level = s;
  }
  return permille_to_duty(level, g->pct);
}

// Advance the output toward the target at the bounded slew rate and
// return the duty to write. Call it at any cadence (the device uses
// 20 ms); a long gap just lets it move further, never jump past target.
inline uint16_t glow_step(GlowState* g, uint32_t now_ms) {
  const uint16_t target = glow_target(g, now_ms);
  uint32_t dt = now_ms - g->last_step_ms;
  g->last_step_ms = now_ms;
  if (dt > 1000) dt = 1000;
  const uint32_t max_move = dt * kSlewPerMs;
  if (target > g->duty) {
    const uint32_t up = target - g->duty;
    g->duty = (uint16_t)(g->duty + (up < max_move ? up : max_move));
  } else {
    const uint32_t down = g->duty - target;
    g->duty = (uint16_t)(g->duty - (down < max_move ? down : max_move));
  }
  return g->duty;
}

// The mode the device's state implies. `enabled` is the owner's switch,
// `stuck` the button's fault, `hub_ok` whether the witness can reach the
// house (its MQTT broker).
inline GlowMode glow_mode_for(bool enabled, bool stuck, bool hub_ok) {
  if (!enabled) return GlowMode::OFF;
  if (stuck) return GlowMode::FAULT;
  return hub_ok ? GlowMode::AWAKE : GlowMode::UNSURE;
}

}  // namespace doorbell
