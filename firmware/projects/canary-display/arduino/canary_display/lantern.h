// include/canary/care/lantern.h — the honest night light (lantern), pure
// model (display_nightstand_line.md §4).
//
// The lantern is the OTHER half of the two-channel night split: the state
// channel (LED / screen glow) stays honest — dark-when-safe, any glow means
// attention — while the lantern is a LAMP: user-summoned, timed-out, warm,
// and deliberately decoupled from fleet state. Its light is a look-engine
// scene, never a semantic color, so it cannot say "safe" by glowing.
//
// Invariants this model enforces (host-tested in tests_host/test_lantern.cpp):
//   - A summon burns out on its own: `minutes` after the summon, dark again.
//   - Attention extinguishes it: the instant the fleet's worst reaches Warn
//     the lantern yields the glass back to the truth — and it stays out
//     until summoned again (an alarm is never followed by mood lighting
//     nobody asked for).
//   - The auto schedule ("lantern hours" — a hallway light that runs through
//     quiet hours) is an EXPLICIT opt-in that trades the dark-means-safe
//     signal for a lamp; it is OFF by default, and the attention override
//     applies to it identically.
//
// Pure logic: no Arduino, no storage, no clock reads — the device glue
// (src/care/lantern.cpp) owns NVS and feeds `now`.
#pragma once
#include <stdint.h>

namespace canary::care {

// Auto-schedule choices. Off is the honest default; Night runs the lantern
// through quiet hours (the hallway-light opt-in).
enum LanternAuto : uint8_t {
  LANTERN_AUTO_OFF   = 0,
  LANTERN_AUTO_NIGHT = 1,
};

class LanternModel {
 public:
  // Preferences (persisted by the glue; seeded from CD_LANTERN_*).
  void configure(uint8_t scene, uint16_t minutes, uint8_t auto_mode) {
    scene_ = scene;
    minutes_ = minutes < 1 ? 1 : minutes;
    auto_mode_ = auto_mode;
  }
  uint8_t scene() const { return scene_; }
  uint16_t minutes() const { return minutes_; }
  uint8_t auto_mode() const { return auto_mode_; }

  // Light it for the configured window from `now`. Re-summoning while lit
  // restarts the window (the natural "keep it on" gesture).
  void summon(uint32_t now_ms) {
    lit_ = true;
    until_ms_ = now_ms + (uint32_t)minutes_ * 60000UL;
  }

  void extinguish() { lit_ = false; }

  // Advance the scene ring (wraps). `scene_count` comes from the caller so
  // the model stays free of the look-engine dependency.
  void cycle_scene(uint8_t scene_count) {
    if (scene_count) scene_ = (uint8_t)((scene_ + 1) % scene_count);
  }

  // The one question the render/brightness paths ask. `attention` is
  // (worst >= Warn) || link-down-honesty — any condition the glass must
  // show; it extinguishes a summon rather than merely masking it.
  bool active(uint32_t now_ms, bool night, bool attention) {
    if (attention) {
      lit_ = false;
      return false;
    }
    if (lit_ && (int32_t)(now_ms - until_ms_) >= 0) lit_ = false;
    if (lit_) return true;
    return auto_mode_ == LANTERN_AUTO_NIGHT && night;
  }

  // True only for a summoned (timed) window — the UI shows the remaining
  // time for these; the auto schedule has no countdown.
  bool summoned(uint32_t now_ms) const {
    return lit_ && (int32_t)(now_ms - until_ms_) < 0;
  }

  // Remaining seconds of a summoned window (0 when not summoned).
  uint32_t remaining_s(uint32_t now_ms) const {
    if (!summoned(now_ms)) return 0;
    return (until_ms_ - now_ms) / 1000UL;
  }

 private:
  uint8_t scene_ = 6;        // look_engine "Lantern" (glue re-seeds from config)
  uint16_t minutes_ = 15;
  uint8_t auto_mode_ = LANTERN_AUTO_OFF;
  bool lit_ = false;
  uint32_t until_ms_ = 0;
};

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
// ── Device glue (src/care/lantern.cpp) ──────────────────────────────────
// The one shared instance + NVS persistence of the preferences (scene,
// minutes, auto mode). Summons are never persisted: a reboot mid-lantern
// wakes dark, which is the honest default.
LanternModel& lantern();
void lantern_begin();                 // NVS restore
void lantern_prefs_changed();         // persist after a scene/auto change
#endif

}  // namespace canary::care
