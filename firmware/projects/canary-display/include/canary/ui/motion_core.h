// include/canary/ui/motion_core.h — the motion engine's pure half.
//
// "The engine knows what the screen is capable of." Every display flavor
// ships different physics — a 240x240 round panel on a 40 MHz SPI wire, an
// 800x480 RGB glass whose scanout is free but whose full-screen repaint is
// not, a PSRAM-less C3 pushing 180x320 over an 80 MHz link — and until now
// every face just guessed. This header turns the raw facts the board pins
// already carry (geometry, bus, clock, PSRAM, CPU) into one small capability
// model, a motion TIER derived from it, easing curves, a frame-time governor
// and the weather-scene math, all of it integer-only and LVGL-free so the
// exact arithmetic that runs on the glass runs in tests_host/test_motion.cpp.
//
// The design law stands: Quiet Glass motion is RATIONED (display_ux_design.md
// §Design language, extended by display_motion_engine.md). This engine does
// not add license to move — it makes the sanctioned motions land the way
// people expect (eased, paced, never janky) and refuses them, by tier or by
// state, on glass that cannot afford them. Decorative motion never outranks
// honesty: an alarm, the night floor, and the lean tier all say "hold still"
// and the engine's answer is always yes.
//
// Time-parameterized on purpose, like common/color/look_engine.h: the loop
// is cooperative and the frame cadence is not guaranteed, so everything here
// is a pure function of (state, now) — never a step counter that drifts when
// a frame drops.
#pragma once
#include <stdint.h>
#include <string.h>

namespace canary::ui::motion {

// ── The capability model ─────────────────────────────────────────────────

// How pixels reach the panel. The distinction that matters for motion is
// whether a repaint PAYS PER PIXEL ON A WIRE (SPI: the bus is the budget) or
// renders into continuously scanned memory (RGB: the render is the budget).
// QspiCmd is the AMOLED watch board's 4-lane command bus (Phase-0 hardware,
// not yet in a display env); Emulated is the WASM Lab, where the browser
// compositor makes bus math meaningless.
enum class Bus : uint8_t {
  Spi = 0,
  RgbPanel = 1,
  QspiCmd = 2,
  Emulated = 3,
};

struct Caps {
  int16_t w = 0, h = 0;        // native panel geometry (px)
  Bus bus = Bus::Spi;
  uint32_t bus_hz = 0;         // SPI clock / RGB pclk; 0 = not bus-bound
  bool psram = false;
  uint16_t cpu_mhz = 0;
  bool lean = false;           // CD_LEAN_BUILD (4 MB C3/C6 class)
  bool pwm_backlight = false;  // HAS_BACKLIGHT_PWM (false = binary expander)
};

// Theoretical cost of pushing one FULL frame over the wire, microseconds.
// SPI only — an RGB panel's scanout is continuous and costs the same whether
// the frame changed or not, so its full-frame figure is 0 ("not bus-bound").
// The 110/100 factor covers command/address overhead between windows.
constexpr uint32_t full_frame_us(const Caps& c) {
  return (c.bus == Bus::Spi && c.bus_hz > 0)
             ? (uint32_t)(((uint64_t)c.w * c.h * 16u * 1000000u / c.bus_hz) *
                          110u / 100u)
             : 0u;
}

// ── The tier ─────────────────────────────────────────────────────────────
//
// Three tiers, derived — never hand-assigned per board, so a new pins.h is
// classified by its physics the day it lands:
//
//   Lean     — no PSRAM, sub-200 MHz, or a CD_LEAN_BUILD flash budget.
//              Micro-animations snap, ambient scenes never run, durations
//              shorten (fewer frames on hardware that renders fewer).
//   Standard — bus-bound SPI glass on a capable chip (the watch, the S3
//              nightstands): small-region motion is cheap, full-frame
//              motion is metered by the wire.
//   Full     — render-bound RGB glass with PSRAM on an S3 (the dash/7"
//              family): the richest grammar, still governed at runtime.
enum class Tier : uint8_t { Lean = 0, Standard = 1, Full = 2 };

// A full-frame flush past this is a panel that cannot carry whole-screen
// effects at animation rate — it still animates, in smaller regions.
constexpr uint32_t FULL_FLUSH_CEILING_US = 18000;

constexpr Tier tier_for(const Caps& c) {
  return (c.lean || !c.psram || c.cpu_mhz < 200)
             ? Tier::Lean
             : ((c.bus == Bus::Spi && full_frame_us(c) > FULL_FLUSH_CEILING_US)
                    ? Tier::Standard
                    : Tier::Full);
}

// ── Duration tokens ──────────────────────────────────────────────────────
//
// Four durations, not a parts bin — the same discipline as the type ladder.
// Micro is the polish class (a digit morph, a value fade); Short is the
// established page fade (= theme.h MOTION_PAGE_MS on every tier that runs
// it); Medium/Long are entrances and reveals. Lean returns 0 for Micro —
// "snap" is a tier's honest answer, and callers write one code path because
// a 0 ms animation IS an assignment.
enum class Dur : uint8_t { Micro = 0, Short = 1, Medium = 2, Long = 3 };

constexpr uint32_t dur_ms(Dur d, Tier t) {
  return (t == Tier::Full)
             ? (d == Dur::Micro ? 160u
                                : d == Dur::Short ? 220u
                                                  : d == Dur::Medium ? 320u
                                                                     : 480u)
             : (t == Tier::Standard)
                   ? (d == Dur::Micro ? 140u
                                      : d == Dur::Short
                                            ? 220u
                                            : d == Dur::Medium ? 280u : 400u)
                   : (d == Dur::Micro ? 0u
                                      : d == Dur::Short
                                            ? 160u
                                            : d == Dur::Medium ? 200u : 280u);
}

// ── Effect classes and the gate ──────────────────────────────────────────
//
// Every motion the engine runs belongs to a class, and the class decides
// who may veto it. This is the "calm technology" table made executable:
//
//   Semantic   — alert breathing, the ack ring, the heartbeat. Meaning
//                carried by motion; never gated (the faces already ration
//                these by their own rules).
//   Transition — ground swaps, page changes. Always allowed (a hard cut is
//                worse orientation than a fade), tier sets the pace.
//   Micro      — digit morphs, value fades, the minute sweep. Polish;
//                yields to an unacked alarm (attention belongs to the
//                alarm, not to a pretty minute flip). Lean snaps via
//                dur_ms(Micro) == 0.
//   Ambient    — decorative standing motion: the weather scene. The most
//                gated class: day only, calm only, no modal open, never on
//                Lean. "A wall object at rest is still" — ambient motion
//                exists only while the glass is actively saying something.
enum class Fx : uint8_t { Semantic = 0, Transition = 1, Micro = 2, Ambient = 3 };

struct Gate {
  bool night = false;          // quiet hours (the mode, not the preference)
  bool alert_unacked = false;  // any unacked Alert/Tamper on the fleet
  bool modal = false;          // settings / commissioning / demo sheet open
};

constexpr bool fx_allowed(Fx f, const Gate& g, Tier t) {
  return (f == Fx::Semantic || f == Fx::Transition)
             ? true
             : (f == Fx::Micro)
                   ? !g.alert_unacked
                   : (t != Tier::Lean && !g.night && !g.alert_unacked &&
                      !g.modal);
}

// ── Easing curves ────────────────────────────────────────────────────────
//
// Fixed-point, t in 0..1024 -> value in 0..1024 (Back overshoots mid-curve,
// lands on 1024). These are the curves people know without knowing them:
// the standard system-animation feel is a strong ease-out (arrive gently,
// depart briskly), ease-in-out for symmetric breathing, and a small damped
// overshoot where a settle should feel physical. Integer math throughout —
// the C3/C6 have no FPU, and look_engine already set the precedent.

constexpr int32_t kMotionOne = 1024;

// Arrive gently: 1 - (1-t)^3.
constexpr int32_t ease_out_cubic(int32_t t) {
  return (t <= 0) ? 0
                  : (t >= kMotionOne)
                        ? kMotionOne
                        : kMotionOne -
                              (int32_t)((int64_t)(kMotionOne - t) *
                                        (kMotionOne - t) / kMotionOne *
                                        (kMotionOne - t) / kMotionOne);
}

// Symmetric: cubic smoothstep-style in-out.
constexpr int32_t ease_in_out_cubic(int32_t t) {
  return (t <= 0) ? 0
                  : (t >= kMotionOne)
                        ? kMotionOne
                        : (t < kMotionOne / 2)
                              ? (int32_t)(4 * (int64_t)t * t / kMotionOne * t /
                                          kMotionOne)
                              : kMotionOne -
                                    (int32_t)(4 *
                                              (int64_t)(kMotionOne - t) *
                                              (kMotionOne - t) / kMotionOne *
                                              (kMotionOne - t) / kMotionOne);
}

// Ease-out with a small physical settle (~4% overshoot, one pass — a
// noticeable "landing", never a bounce). s = 0.7 in Q10.
constexpr int32_t kBackS = 716;

constexpr int32_t ease_out_back(int32_t t) {
  // u = t-1 in Q10 (negative); f = 1 + (s+1)*u^3 + s*u^2
  return (t <= 0)
             ? 0
             : (t >= kMotionOne)
                   ? kMotionOne
                   : (int32_t)(kMotionOne +
                               ((kBackS + kMotionOne) *
                                    ((int64_t)(t - kMotionOne) *
                                     (t - kMotionOne) / kMotionOne *
                                     (t - kMotionOne) / kMotionOne) +
                                kBackS * ((int64_t)(t - kMotionOne) *
                                          (t - kMotionOne) / kMotionOne)) /
                                   kMotionOne);
}

// Gentle glide (backlight ramps): 1 - (1-t)^2.
constexpr int32_t ease_out_quad(int32_t t) {
  return (t <= 0) ? 0
                  : (t >= kMotionOne)
                        ? kMotionOne
                        : kMotionOne -
                              (int32_t)((int64_t)(kMotionOne - t) *
                                        (kMotionOne - t) / kMotionOne);
}

// Interpolate a..b along an eased t (curve output). Handles a > b.
constexpr int32_t lerp1024(int32_t a, int32_t b, int32_t v) {
  return a + (int32_t)((int64_t)(b - a) * v / kMotionOne);
}

// ── Integer sine (for sway/drift) ────────────────────────────────────────
// Quarter table, 64 steps; angle 0..1023 = one full turn; returns
// -1024..1024. Accuracy is visual-grade on purpose.
inline int32_t sin1024(uint32_t angle) {
  static const int16_t kQ[65] = {
      0, 25, 50, 75, 100, 125, 150, 175, 200, 224, 249, 273, 297,
      321, 345, 369, 392, 415, 438, 460, 483, 505, 526, 548, 569, 590,
      610, 630, 650, 669, 688, 706, 724, 742, 759, 775, 792, 807, 822,
      837, 851, 865, 878, 891, 903, 915, 926, 936, 946, 955, 964, 972,
      980, 987, 993, 999, 1004, 1009, 1013, 1016, 1019, 1021, 1023, 1024, 1024,
  };
  const uint32_t a = angle & 1023u;
  const uint32_t q = a >> 8;          // quadrant 0..3
  const uint32_t k = a & 255u;        // position in quadrant, 0..255
  const uint32_t idx = (q & 1u) ? (256u - k) : k;   // mirror odd quadrants
  const int32_t v = kQ[idx >> 2];     // 0..64 table entries
  return (q >= 2u) ? -v : v;
}

// ── The frame-time governor ──────────────────────────────────────────────
//
// The runtime half of "knows what the hardware does": the loop samples how
// long each lv_timer_handler pass actually took and the governor turns that
// into a quality level for the DECORATIVE budget only — semantic motion is
// never traded away. Degrade is fast (a few heavy frames in a row), recover
// is slow (sustained light frames) so quality never flaps at a boundary.
//
//   level 0 — rich:    the tier's full grammar
//   level 1 — trimmed: ambient scenes pause, micro-motion stays
//   level 2 — still:   decorative motion parks entirely
struct Governor {
  int32_t ewma_us = 0;       // smoothed frame time
  uint8_t level = 0;         // 0 rich / 1 trimmed / 2 still
  uint8_t over_streak = 0;   // consecutive heavy frames
  uint16_t calm_frames = 0;  // consecutive light frames toward recovery
};

constexpr uint8_t GOV_LEVELS = 3;
constexpr uint8_t GOV_DEGRADE_STREAK = 4;
constexpr uint16_t GOV_RECOVER_FRAMES = 240;   // ~4-8 s at animation rate

// Per-tier frame budget: what one handler pass may cost before the frame
// counts as heavy. Derived from the refresh ceiling each tier runs at, with
// headroom for the loop's other tenants (MQTT, HTTP, OTA share the task).
constexpr uint32_t frame_budget_us(Tier t) {
  return (t == Tier::Full) ? 22000u : (t == Tier::Standard) ? 26000u : 40000u;
}

// Feed one frame sample; returns the (possibly changed) level.
inline uint8_t governor_step(Governor& g, uint32_t frame_us,
                             uint32_t budget_us) {
  g.ewma_us += ((int32_t)frame_us - g.ewma_us) / 8;
  const bool heavy = frame_us > budget_us + budget_us / 2;   // 1.5x budget
  const bool light = g.ewma_us < (int32_t)(budget_us * 3 / 5);
  if (heavy) {
    g.calm_frames = 0;
    if (g.over_streak < 255) g.over_streak++;
    if (g.over_streak >= GOV_DEGRADE_STREAK && g.level < GOV_LEVELS - 1) {
      g.level++;
      g.over_streak = 0;
    }
  } else {
    g.over_streak = 0;
    if (light && g.level > 0) {
      if (++g.calm_frames >= GOV_RECOVER_FRAMES) {
        g.level--;
        g.calm_frames = 0;
      }
    } else {
      g.calm_frames = 0;
    }
  }
  return g.level;
}

// ── The weather scene ────────────────────────────────────────────────────
//
// Live weather movement, from the words the wire already carries (wx_core's
// plain-language conditions — no icon vocabulary, no new wire fields). A
// scene is a handful of soft shapes whose positions are pure functions of
// (seed, index, now): rain falls, snow sways, clouds drift in parallax, fog
// breathes sideways. Deterministic by construction — the same second on the
// same device renders the same field, and the host tests walk the math.
enum class WxScene : uint8_t {
  Still = 0,   // clear / mostly clear / unknown: a wall object at rest
  Clouds = 1,
  Rain = 2,
  Snow = 3,
  Fog = 4,
};

// The condition words are wx_core.h's closed set; anything unknown is Still.
inline WxScene wx_scene_for_word(const char* cond) {
  if (!cond || !cond[0]) return WxScene::Still;
  if (!strcmp(cond, "some clouds") || !strcmp(cond, "overcast"))
    return WxScene::Clouds;
  if (!strcmp(cond, "drizzle") || !strcmp(cond, "rain") ||
      !strcmp(cond, "showers") || !strcmp(cond, "thunderstorm"))
    return WxScene::Rain;
  if (!strcmp(cond, "snow") || !strcmp(cond, "snow showers"))
    return WxScene::Snow;
  if (!strcmp(cond, "fog")) return WxScene::Fog;
  return WxScene::Still;   // "clear", "mostly clear", and the future
}

// Particle budget per tier. Full carries the whole field; Standard halves
// it; Lean (and Still) carry none. These are budgets, not resolutions —
// the scene reads at three shapes, it just reads richer at fourteen.
inline int wx_particle_count(WxScene s, Tier t) {
  if (t == Tier::Lean) return 0;
  int n = 0;
  switch (s) {
    case WxScene::Clouds: n = 3; break;
    case WxScene::Rain:   n = 14; break;
    case WxScene::Snow:   n = 10; break;
    case WxScene::Fog:    n = 3; break;
    default:              n = 0; break;
  }
  return (t == Tier::Standard) ? (n + 1) / 2 : n;
}

struct WxParticle {
  int16_t x = 0, y = 0;   // top-left within the scene field
  uint8_t opa = 0;        // 0..255
  uint8_t px = 0;         // characteristic size (the glue scales per kind)
};

// Small deterministic mixer (xorshift-style) — per-particle personality
// without storing any state.
constexpr uint32_t wx_hash(uint32_t x) {
  return ((x ^ (x << 13)) ^ ((x ^ (x << 13)) >> 17)) ^
         (((x ^ (x << 13)) ^ ((x ^ (x << 13)) >> 17)) << 5);
}

// Position particle i of scene s at time now within a w x h field.
inline WxParticle wx_particle(WxScene s, uint32_t seed, int i, uint32_t now_ms,
                              int16_t w, int16_t h) {
  WxParticle p;
  if (w <= 0 || h <= 0) return p;
  const uint32_t h1 = wx_hash(seed + (uint32_t)i * 2654435761u);
  const uint32_t h2 = wx_hash(h1);
  switch (s) {
    case WxScene::Rain: {
      // Fast fall with a constant slant; per-drop period and lane.
      const uint32_t period = 900u + (h1 & 255u);            // 900..1155 ms
      const uint32_t ph = (now_ms + h2) % period;
      p.y = (int16_t)((int32_t)ph * h / (int32_t)period);
      const int32_t lane = (int32_t)(h1 % (uint32_t)w);
      int32_t x = lane - p.y / 6;                            // slanted fall
      x %= w; if (x < 0) x += w;
      p.x = (int16_t)x;
      p.opa = (uint8_t)(110u + (h2 & 63u));
      p.px = (uint8_t)(10u + (h1 & 7u));                     // streak length
      break;
    }
    case WxScene::Snow: {
      const uint32_t period = 4200u + (h1 & 2047u);          // 4.2..6.2 s
      const uint32_t ph = (now_ms + h2) % period;
      p.y = (int16_t)((int32_t)ph * h / (int32_t)period);
      const int32_t lane = (int32_t)(h1 % (uint32_t)w);
      const int32_t sway =
          sin1024((now_ms / 3u + (h2 & 1023u)) & 1023u) * (w / 20) / 1024;
      int32_t x = lane + sway;
      x %= w; if (x < 0) x += w;
      p.x = (int16_t)x;
      p.opa = (uint8_t)(140u + (h2 & 63u));
      p.px = (uint8_t)(3u + (h1 & 3u));                      // flake diameter
      break;
    }
    case WxScene::Clouds: {
      // Three depth layers drifting the same way; nearer = faster, denser.
      const int layer = i % 3;
      const int32_t cw = w / 2;                              // blob width
      const uint32_t period = 42000u + (uint32_t)layer * 16000u + (h1 & 4095u);
      const uint32_t ph = (now_ms + h2) % period;
      p.x = (int16_t)((int32_t)ph * (w + cw) / (int32_t)period - cw);
      p.y = (int16_t)(h * (1 + 2 * layer) / 8);
      p.opa = (uint8_t)(30 - 7 * layer);
      p.px = (uint8_t)(layer);                               // depth index
      break;
    }
    case WxScene::Fog: {
      // Wide bands breathing sideways, slowly.
      const int band = i % 3;
      p.y = (int16_t)(h * (1 + band) / 4);
      const int32_t off =
          sin1024((now_ms / 12u + (uint32_t)band * 341u) & 1023u) * (w / 12) /
          1024;
      p.x = (int16_t)(-(w / 10) + off);
      p.opa = (uint8_t)(22 - 4 * band);
      p.px = (uint8_t)(band);
      break;
    }
    default:
      break;
  }
  return p;
}

// The engine timer's period for a scene, by tier — how often the field
// repositions. Rain wants animation rate; clouds and fog crawl.
inline uint32_t wx_tick_ms(WxScene s, Tier t) {
  const bool fast = (s == WxScene::Rain || s == WxScene::Snow);
  if (t == Tier::Full) return fast ? 33u : 120u;
  return fast ? 66u : 200u;
}

}  // namespace canary::ui::motion
