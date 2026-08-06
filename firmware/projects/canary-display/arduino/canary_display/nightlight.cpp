// src/care/nightlight.cpp — nightlight device glue: the shared visits
// instance + NVS persistence of the two nightlight prefs (12-hour clock,
// lamp strength). The visits model itself is pure and host-tested — see
// include/canary/care/nightlight.h.
#include "flavor_config.h"

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <Preferences.h>

#ifdef CD_NIGHTLIGHT

#include "nightlight_glue.h"

namespace canary::care {

namespace {
NightlightVisits s_visits;
constexpr const char* NVS_NS = "scv-nl";
bool s_12h = true;
uint8_t s_bri = 184;   // the look engine's own default (~72%)
uint8_t s_rot = 0;     // rendered rotation, 0..3 quarter turns CW
bool s_arot = true;    // the IMU follows real movement by default
int s_rot_req = -1;    // cross-context request (web handler -> main loop)

void put_u8(const char* key, uint8_t v) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar(key, v);
  p.end();
}
}  // namespace

NightlightVisits& nightlight_visits() { return s_visits; }

void nightlight_begin() {
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    s_12h = p.getUChar("12h", 1) != 0;
    s_bri = (uint8_t)p.getUChar("bri", s_bri);
    s_rot = (uint8_t)(p.getUChar("rot", 0) & 3);
    s_arot = p.getUChar("arot", 1) != 0;
    p.end();
  }
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

uint8_t nightlight_rotation() { return s_rot; }

void nightlight_set_rotation(uint8_t rot) {
  rot &= 3;
  if (rot == s_rot) return;
  s_rot = rot;
  put_u8("rot", s_rot);
}

bool nightlight_auto_rotate() { return s_arot; }

void nightlight_set_auto_rotate(bool on) {
  if (on == s_arot) return;
  s_arot = on;
  put_u8("arot", on ? 1 : 0);
}

void nightlight_request_rotation(uint8_t rot) { s_rot_req = rot & 3; }

int nightlight_take_rotation_request() {
  const int r = s_rot_req;
  s_rot_req = -1;
  return r;
}

}  // namespace canary::care

#endif  // CD_NIGHTLIGHT
