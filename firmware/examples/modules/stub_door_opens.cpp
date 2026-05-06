/**
 * @file stub_door_opens.cpp
 * @brief Example: simple "door opens" event detector.
 *
 * Signature: one window with motion >= MOTION_SPIKE, then within
 * QUIET_WINDOW windows the motion drops below MOTION_QUIET. Emits one
 * `door_event` per detection. Bundling and ceiling enforcement come for
 * free from the chokepoint.
 *
 * The numbers are illustrative; if you adopt this seriously, route them
 * through a Tuning Lab knob.
 */

#include "stub_door_opens.h"
#include <csi_event.h>

#include <string.h>

namespace {

constexpr uint8_t  MOTION_SPIKE  = 80;
constexpr uint8_t  MOTION_QUIET  = 20;
constexpr uint16_t QUIET_WINDOW  = 4;

bool     s_armed       = false;
uint16_t s_quiet_count = 0;

uint8_t reduce_doppler(const int8_t* v) {
  int32_t s = 0;
  for (int i = 8; i < 12; ++i) {
    int8_t b = v[i];
    s += (b < 0) ? -(int32_t)b : (int32_t)b;
  }
  int32_t avg = s / 4;
  if (avg > 127) avg = 127;
  return (uint8_t)avg;
}

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "door_event",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_MOTION_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 30,   /* a busy hallway might fire often */
  },
};

void on_init(const csi_module_settings_t* /*s*/) {
  s_armed = false;
  s_quiet_count = 0;
}

void emit_door_event(uint8_t peak) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_CONFIDENCE
                   | CSI_FIELD_TIME_BUCKET | CSI_FIELD_MOTION_SCORE;
  strncpy(v.state_name, "door",      sizeof(v.state_name) - 1);
  strncpy(v.confidence, "observed",  sizeof(v.confidence) - 1);
  v.motion_score = peak;
  (void)csi_event_emit("third.door_opens", "door_event", &v);
}

void on_tick(const csi_features_t* f) {
  if (!f) return;
  const uint8_t motion = reduce_doppler(f->v);

  if (!s_armed) {
    if (motion >= MOTION_SPIKE) {
      s_armed = true;
      s_quiet_count = 0;
    }
    return;
  }
  if (motion < MOTION_QUIET) {
    if (++s_quiet_count >= QUIET_WINDOW) {
      emit_door_event(MOTION_SPIKE);
      s_armed = false;
      s_quiet_count = 0;
    }
  } else {
    /* Motion did not actually drop — likely a person, not a door. */
    if (motion < MOTION_SPIKE) s_quiet_count = 0;
    else s_armed = true;
  }
}

const csi_module_t MODULE = {
  /* id */                 "third.door_opens",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             nullptr,
};

}  /* namespace */

extern "C" const csi_module_t* stub_door_opens_module(void) { return &MODULE; }
