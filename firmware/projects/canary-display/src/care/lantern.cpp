// src/care/lantern.cpp — lantern device glue: the shared instance + NVS
// persistence of the lantern preferences. The model itself (timeout, the
// attention veto, the auto schedule) is pure and host-tested — see
// include/canary/care/lantern.h.
#include <config.h>

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <Preferences.h>

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN

#include "canary/care/lantern.h"
// The chosen hue lives with the other look controls, and is persisted here
// beside the lamp's scene and timer because it is the same kind of setting:
// something the owner picked and expects to still be true after a power cut.
#include "canary/ui/look_state.h"

namespace canary::care {

namespace {
LanternModel s_lantern;
constexpr const char* NVS_NS = "scv-lantern";

#ifndef CD_LANTERN_SCENE
#define CD_LANTERN_SCENE 6
#endif
#ifndef CD_LANTERN_MINUTES
#define CD_LANTERN_MINUTES 15
#endif
#ifndef CD_LANTERN_AUTO
#define CD_LANTERN_AUTO 0
#endif
}  // namespace

LanternModel& lantern() { return s_lantern; }

void lantern_begin() {
  Preferences p;
  uint8_t scene = CD_LANTERN_SCENE;
  uint16_t minutes = CD_LANTERN_MINUTES;
  uint8_t auto_mode = CD_LANTERN_AUTO;
  // -1 is "no wheel, use the scene" — the same meaning the API and the look
  // engine give it, stored as +1 so it fits an unsigned NVS slot without a
  // second key to say "is there one".
  int16_t hue = -1;
  if (p.begin(NVS_NS, true)) {
    scene = (uint8_t)p.getUChar("scene", scene);
    minutes = (uint16_t)p.getUShort("min", minutes);
    auto_mode = (uint8_t)p.getUChar("auto", auto_mode);
    hue = (int16_t)p.getUShort("hue", 0) - 1;
    p.end();
  }
  s_lantern.configure(scene, minutes, auto_mode);
  // A color the owner chose has to survive the power cut that made them
  // reboot in the first place — otherwise the picker is a demo, not a setting.
  canary::ui::look_set_custom_hue(hue);
}

void lantern_prefs_changed() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("scene", s_lantern.scene());
  p.putUShort("min", s_lantern.minutes());
  p.putUChar("auto", s_lantern.auto_mode());
  // Stored +1 so "none" is 0 and a real hue is 1..360 (see lantern_begin).
  const int16_t hue = canary::ui::look_params().custom_hue;
  p.putUShort("hue", (uint16_t)(hue < 0 ? 0 : hue + 1));
  p.end();
}

}  // namespace canary::care

#endif  // FEATURE_LANTERN
