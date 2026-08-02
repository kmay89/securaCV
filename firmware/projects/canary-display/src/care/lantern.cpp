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
  if (p.begin(NVS_NS, true)) {
    scene = (uint8_t)p.getUChar("scene", scene);
    minutes = (uint16_t)p.getUShort("min", minutes);
    auto_mode = (uint8_t)p.getUChar("auto", auto_mode);
    p.end();
  }
  s_lantern.configure(scene, minutes, auto_mode);
}

void lantern_prefs_changed() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("scene", s_lantern.scene());
  p.putUShort("min", s_lantern.minutes());
  p.putUChar("auto", s_lantern.auto_mode());
  p.end();
}

}  // namespace canary::care

#endif  // FEATURE_LANTERN
