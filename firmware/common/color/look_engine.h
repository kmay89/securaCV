// firmware/common/color/look_engine.h — the Nightstand "look" engine.
//
// Turns a chosen scene + motion + brightness/warmth/gamma + the fleet's worst
// severity into (a) the single RGB the WS2812 beacon should show this instant,
// and (b) the vertical gradient stops the glass should wash with. Board- and
// framework-agnostic integer math, so it is unit-tested on the host and runs
// unchanged on the FPU-less C6 and the S3.
//
// The one inviolable rule (mirrors the whole product): a SCENE only ever
// dresses the CALM. The moment the fleet's worst severity reaches Warn, the
// engine abandons the scene and returns the true SEMANTIC color (amber / red),
// so the look can never hide an alarm. And `safe_dark` (night + all-quiet +
// links healthy) returns black — darkness at night still means safe.
#pragma once
#include <stdint.h>
#include "color/color_engine.h"

namespace canary::color {

// Gradient motion styles (the "Effect" control).
enum class Motion : uint8_t { Breathe, Sweep, Shimmer, Pulse, Comet };

// A named scene: a small looping HSV palette + its default motion.
struct Scene {
  const char* id;
  const char* name;
  Motion motion;
  uint8_t n_stops;
  Hsv stops[4];
};

// Severity, integer-compatible with canary::fleet::Sev (same order) so a
// caller can pass its fleet severity straight through as a cast.
enum class Sev : uint8_t { Ok = 0, Notice, Warn, Alert, Tamper };

// The live look controls (settings surface / MQTT own these on-device).
struct LookParams {
  uint8_t scene_idx = 0;
  bool    motion_use_scene = true;   // true = the scene's own motion
  Motion  motion = Motion::Breathe;  // used when motion_use_scene == false
  uint8_t brightness = 184;          // 0..255 (~72%)
  uint8_t speed = 128;               // 0..255, 128 = 1.0x
  int8_t  warmth = 0;                // -100..100
  bool    gamma_on = true;
  bool    night = false;
};

// The scene catalog (defined in look_engine.cpp).
extern const Scene kScenes[];
extern const uint8_t kSceneCount;

// The beacon color for this instant. Returns {0,0,0} when safe_dark.
Rgb led_color(uint32_t now_ms, const LookParams& p, Sev worst, bool safe_dark);

// Fill `out[0..count)` with the glass wash gradient stops (top→bottom) for
// this instant. When worst >= Warn the stops carry the semantic color; when
// safe_dark they are all black. `count` should be >= 2.
void wash_stops(uint32_t now_ms, const LookParams& p, Sev worst,
                bool safe_dark, Rgb* out, uint8_t count);

}  // namespace canary::color
