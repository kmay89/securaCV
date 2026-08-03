// src/care/hallway.cpp — Hallway mode device glue: the shared instance, NVS
// persistence, and the ONE function that applies the preset.
//
// The model itself (the dwell envelope, the attention veto, the level presets)
// is pure and host-tested — see include/canary/care/hallway.h. What lives here
// is the part that touches the device: NVS, the lantern's auto mode, and the
// shared LookParams.
//
// `hallway_set()` is deliberately the only writer. The settings surface, an
// MQTT topic and the BOOT button all call it, so there is exactly one code
// path that turns lantern hours on and exactly one that turns them off — which
// is what stops the two from drifting into disagreeing about what "hallway
// mode" left behind.
#include "flavor_config.h"

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <Preferences.h>

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN

#include "hallway.h"
#include "lantern.h"

namespace canary::care {

namespace {
HallwayModel s_hallway;
constexpr const char* NVS_NS = "scv-hallway";

#ifndef CD_HALLWAY_ON
#define CD_HALLWAY_ON 0
#endif
#ifndef CD_HALLWAY_LEVEL
#define CD_HALLWAY_LEVEL 1 /* HALLWAY_SOFT */
#endif
#ifndef CD_HALLWAY_RISE_MIN
#define CD_HALLWAY_RISE_MIN 6
#endif
#ifndef CD_HALLWAY_EBB_MIN
#define CD_HALLWAY_EBB_MIN 12
#endif
#ifndef CD_HALLWAY_SCENE
#define CD_HALLWAY_SCENE 6 /* look_engine kScenes[6] = "Lantern" */
#endif
#ifndef CD_HALLWAY_BEACON
#define CD_HALLWAY_BEACON 1 /* the WS2812 joins a LIT hallway lamp — see hallway.h */
#endif

// Push the switch onto the one thing it actually changes: the lantern's auto
// schedule. Called on boot and on every flip.
//
// The LOOK (brightness, warmth, plumage depth) is deliberately NOT written
// here. The render path reads `hallway().preset()` per frame and scales it by
// the dwell envelope, which it has to do anyway — writing the same numbers
// into the shared LookParams as well would give the feature two sources of
// truth that disagree the moment the dwell is anywhere but full.
//
// Turning Hallway mode OFF restores the lantern's auto mode to OFF rather
// than to whatever it was before — the honest default. A switch that could
// leave lantern hours running after you turned the hallway light off would be
// a switch that quietly keeps costing you the dark-means-safe signal.
void apply() {
  auto& lamp = lantern();
  lamp.configure(s_hallway.enabled() ? (uint8_t)CD_HALLWAY_SCENE : lamp.scene(),
                 lamp.minutes(),
                 s_hallway.enabled() ? (uint8_t)LANTERN_AUTO_NIGHT
                                     : (uint8_t)LANTERN_AUTO_OFF);
  lantern_prefs_changed();
}
}  // namespace

HallwayModel& hallway() { return s_hallway; }

void hallway_begin() {
  Preferences p;
  bool on = CD_HALLWAY_ON;
  uint8_t level = CD_HALLWAY_LEVEL;
  uint16_t rise = CD_HALLWAY_RISE_MIN;
  uint16_t ebb = CD_HALLWAY_EBB_MIN;
  bool beacon = CD_HALLWAY_BEACON;
  if (p.begin(NVS_NS, true)) {
    on = p.getBool("on", on);
    level = (uint8_t)p.getUChar("level", level);
    rise = (uint16_t)p.getUShort("rise", rise);
    ebb = (uint16_t)p.getUShort("ebb", ebb);
    beacon = p.getBool("beacon", beacon);
    p.end();
  }
  s_hallway.configure(on, level, rise, ebb, beacon);
  apply();
}

void hallway_set(bool on, uint8_t level) {
  // The switch flips `on` and the level; the ramps and the beacon choice are
  // carried through untouched, so toggling the mode never quietly rewrites
  // preferences the user set separately.
  s_hallway.configure(on, level, s_hallway.rise_min(), s_hallway.ebb_min(),
                      s_hallway.beacon_pref());
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.putBool("on", s_hallway.enabled());
    p.putUChar("level", s_hallway.level_choice());
    p.putUShort("rise", s_hallway.rise_min());
    p.putUShort("ebb", s_hallway.ebb_min());
    p.putBool("beacon", s_hallway.beacon_pref());
    p.end();
  }
  apply();
}

}  // namespace canary::care

#endif  // FEATURE_LANTERN
