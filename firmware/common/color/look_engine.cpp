// firmware/common/color/look_engine.cpp — scenes + the look engine.
// See look_engine.h. Integer-only; hosted-testable.
#include "color/look_engine.h"

namespace canary::color {

// ── The scene catalog (HSV: h 0..359, s/v 0..255) ───────────────────────────
// Hue-inspired, Canary-honest. Kept small (4 stops) so a full glass gradient
// costs a handful of integer samples per frame.
const Scene kScenes[] = {
  {"dawn",     "Canary Dawn", Motion::Breathe, 4,
     {{48,217,255},{36,179,255},{14,140,242},{330,89,230}}},
  {"ember",    "Ember",       Motion::Sweep,   4,
     {{8,230,217},{22,242,255},{40,217,255},{16,230,179}}},
  {"aurora",   "Aurora",      Motion::Sweep,   4,
     {{160,204,217},{140,191,242},{190,179,230},{260,166,204}}},
  {"calm",     "Deep Calm",   Motion::Breathe, 4,
     {{230,191,179},{210,179,217},{250,153,153},{200,179,191}}},
  {"forest",   "Forest",      Motion::Shimmer, 4,
     {{130,204,179},{95,217,230},{160,179,204},{110,204,191}}},
  {"twilight", "Tropical",    Motion::Sweep,   4,
     {{320,191,242},{12,217,255},{38,204,255},{180,179,230}}},
  {"lantern",  "Lantern",     Motion::Breathe, 4,
     {{40,102,153},{42,179,230},{38,217,255},{44,140,191}}},
  {"nocturne", "Nocturne",    Motion::Pulse,   4,
     {{260,179,89},{280,153,128},{240,166,64},{300,140,102}}},
  {"signal",   "Signal",      Motion::Breathe, 4,
     {{122,153,158},{122,140,128},{122,153,153},{122,128,140}}},
  // The full wheel, cycling. Stops 90° apart so shortest-path interpolation
  // walks the whole circle; Sweep scrolls it. The honest override applies to
  // this scene exactly like every other: it dresses the calm only.
  {"rainbow",  "Rainbow",     Motion::Sweep,   4,
     {{0,230,217},{90,230,217},{180,230,217},{270,230,217}}},
  // Bright white, kept a touch warm (a true 0-saturation white reads blue
  // on these panels) with a faint breathing shimmer so the lamp still feels
  // alive. Appended last so every stored scene index keeps its meaning.
  // "Bright" is relative to the device's own ceiling: on the nightlight the
  // HAL caps backlight duty at 50% (heat), and this scene is as bright as
  // that budget allows.
  {"moonbeam", "Moonbeam",    Motion::Breathe, 4,
     {{40,26,255},{42,15,242},{38,20,255},{40,31,248}}},
};
const uint8_t kSceneCount = (uint8_t)(sizeof(kScenes) / sizeof(kScenes[0]));

namespace {

// Semantic (honest) hues — the same timeline-card colors the rest of the
// glass speaks. Day and a dim night-shifted ember.
constexpr Hsv SEM_WARN       = {33, 255, 251};   // amber  #FB8C00
constexpr Hsv SEM_ALERT      = { 1, 196, 229};   // red    #E53935
constexpr Hsv SEM_WARN_NIGHT = { 6, 220,  80};
constexpr Hsv SEM_ALERT_NIGHT= { 2, 230,  92};

uint8_t clamp_speed(uint8_t s) { return s == 0 ? 1 : s; }

// Palette phase 0..255 for this instant: Sweep scrolls a full loop briskly;
// the calmer motions drift slowly so the hue barely moves and the breath does
// the talking. speed 128 = 1.0x.
uint8_t phase_for(uint32_t now, uint8_t speed, Motion m) {
  uint32_t loop_ms = (m == Motion::Sweep) ? 16000u : 60000u;
  loop_ms = loop_ms * 128u / clamp_speed(speed);
  if (loop_ms == 0) loop_ms = 1;
  return (uint8_t)((now % loop_ms) * 256u / loop_ms);
}

uint32_t half_for(uint8_t speed, Motion m, Sev worst) {
  uint32_t base = (m == Motion::Comet) ? 1300u
                : (m == Motion::Shimmer) ? 1400u : 1500u;
  if (worst >= Sev::Alert) base = 1100u;
  else if (worst >= Sev::Warn) base = 1250u;
  return base * 128u / clamp_speed(speed);
}

Hsv semantic(Sev worst, bool night) {
  if (worst >= Sev::Alert)
    return night ? SEM_ALERT_NIGHT : SEM_ALERT;
  return night ? SEM_WARN_NIGHT : SEM_WARN;
}

// Apply the shared value shaping (breath depth, brightness, night dim) to a
// base V, returning the scaled V.
uint8_t shape_v(uint8_t v, uint32_t now, uint32_t half, Sev worst,
                uint8_t brightness, bool night) {
  const uint8_t br = breath(now, half);
  const uint16_t floorV = (worst >= Sev::Warn) ? 100 : 150;   // breath depth
  const uint16_t vmul = (uint16_t)(floorV + (uint32_t)(255 - floorV) * br / 255);
  uint32_t out = (uint32_t)v * vmul / 255;
  out = out * brightness / 255;
  if (night) out = out * 120 / 255;
  return (uint8_t)(out > 255 ? 255 : out);
}

}  // namespace

Scene custom_scene(int16_t hue) {
  const uint16_t h = (uint16_t)(((hue % 360) + 360) % 360);
  // Neighbors a few degrees either side, with the same gentle s/v drift the
  // curated scenes use — enough to breathe, not enough to read as a second
  // color. Deliberately narrow: the owner asked for THIS color.
  const uint16_t h1 = (uint16_t)((h + 6) % 360);
  const uint16_t h2 = (uint16_t)((h + 354) % 360);
  Scene sc{"custom", "Your color", Motion::Breathe, 4,
           {{h, 217, 255}, {h1, 191, 236}, {h, 230, 246}, {h2, 204, 226}}};
  return sc;
}

// One place decides scene-or-custom, so every surface that draws the look —
// the beacon, the glass wash, the plumage overlay — agrees about which one
// is on.
Scene current_look(const LookParams& p) {
  return p.custom_hue >= 0 ? custom_scene(p.custom_hue)
                           : kScenes[p.scene_idx % kSceneCount];
}

namespace {
inline Motion motion_for(const LookParams& p) {
  return p.motion_use_scene ? current_look(p).motion : p.motion;
}
}  // namespace

Rgb led_color(uint32_t now_ms, const LookParams& p, Sev worst, bool safe_dark) {
  if (safe_dark) return {0, 0, 0};

  const Motion m = motion_for(p);
  const uint32_t half = half_for(p.speed, m, worst);

  Hsv base;
  if (worst >= Sev::Warn) {
    base = semantic(worst, p.night);   // the honest override
  } else {
    const Scene sc = current_look(p);
    base = palette_sample(sc.stops, sc.n_stops, phase_for(now_ms, p.speed, m));
  }
  base.v = shape_v(base.v, now_ms, half, worst, p.brightness, p.night);
  return finish(hsv_to_rgb(base.h, base.s, base.v), p.warmth, p.gamma_on);
}

void wash_stops(uint32_t now_ms, const LookParams& p, Sev worst,
                bool safe_dark, Rgb* out, uint8_t count) {
  if (count == 0) return;
  if (safe_dark) {
    for (uint8_t i = 0; i < count; i++) out[i] = {0, 0, 0};
    return;
  }
  const Motion m = motion_for(p);
  const uint32_t half = half_for(p.speed, m, worst);
  const uint8_t phase = phase_for(now_ms, p.speed, m);
  const bool attention = worst >= Sev::Warn;
  const Scene sc = current_look(p);
  const Hsv sem = attention ? semantic(worst, p.night) : Hsv{0, 0, 0};

  for (uint8_t i = 0; i < count; i++) {
    // Top→bottom position across the palette (looping), scrolled by phase.
    const uint8_t t = (uint8_t)(((uint16_t)i * 255 / (count > 1 ? count - 1 : 1)
                                 + phase) & 0xFF);
    Hsv h = attention ? sem : palette_sample(sc.stops, sc.n_stops, t);
    // Attention wash ramps V down the glass a touch so it reads as a field,
    // not a flat block; scene wash breathes as one.
    if (attention) h.v = (uint8_t)((uint32_t)h.v * (200 - (uint32_t)i * 60 / count) / 255 + 30);
    h.v = shape_v(h.v, now_ms, half, worst, p.brightness, p.night);
    out[i] = finish(hsv_to_rgb(h.h, h.s, h.v), p.warmth, p.gamma_on);
  }
}

}  // namespace canary::color
