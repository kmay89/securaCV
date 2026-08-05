// src/care/nightlight.cpp — nightlight device glue: the shared visits
// instance + NVS persistence of the two nightlight prefs (12-hour clock,
// lamp strength). The visits model itself is pure and host-tested — see
// include/canary/care/nightlight.h.
#include <config.h>

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <Preferences.h>

#ifdef CD_NIGHTLIGHT

#include "canary/care/nightlight_glue.h"

namespace canary::care {

namespace {
NightlightVisits s_visits;
constexpr const char* NVS_NS = "scv-nl";
bool s_12h = true;
uint8_t s_bri = 184;   // the look engine's own default (~72%)
}  // namespace

NightlightVisits& nightlight_visits() { return s_visits; }

void nightlight_begin(uint32_t seed) {
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    s_12h = p.getUChar("12h", 1) != 0;
    s_bri = (uint8_t)p.getUChar("bri", s_bri);
    p.end();
  }
  s_visits.seed(seed);
}

bool nightlight_clock_12h() { return s_12h; }

void nightlight_set_clock_12h(bool on) {
  if (on == s_12h) return;
  s_12h = on;
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("12h", s_12h ? 1 : 0);
  p.end();
}

uint8_t nightlight_lamp_bri() { return s_bri; }

void nightlight_set_lamp_bri(uint8_t bri) {
  if (bri == s_bri) return;
  s_bri = bri;
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("bri", s_bri);
  p.end();
}

}  // namespace canary::care

#endif  // CD_NIGHTLIGHT
