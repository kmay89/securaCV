/*
 * SecuraCV Canary — the camera's thermal step
 *
 * What CameraManager::checkThermal() does with one die-temperature check,
 * lifted out of it so the host can run it (sweep F432,
 * firmware/tests_host/test_camera_thermal.cpp): the miss counter, the
 * fail-safe throttle it raises at THERMAL_FAIL_SAFE_COUNT consecutive
 * misses, the 70/80 °C hysteresis (canary_config.h's thresholds, shared
 * with the thermal watchdog's shadow classifier), and the frame delay each
 * state paces at. Pure: no Arduino, no driver. The camera keeps the check
 * interval and the log lines; this answers what changed.
 *
 * A chip with no die sensor is not a chip whose sensor fails. On the classic
 * ESP32 (env:esp32cam, env:esp32-wroom) securacv_thermal.cpp has no sensor
 * to drive (SECURACV_HAVE_TSENS 0), thermal_read_die_c() always answers
 * false and thermal_sensor_present() says so. check() asks present() first
 * and reads nothing when it is false: no miss is counted, the fail-safe
 * never fires, and the state stays where reset() put it. Before, the third
 * miss throttled the first PEEK stream after boot and logged "Camera
 * thermal sensor read failing" for a sensor the board does not have.
 * sensor_ok() is false there too, not ok: there is no sensor to vouch for.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CAMERA_THERMAL_H
#define SECURACV_CAMERA_THERMAL_H

#include <stdint.h>

#include "canary_config.h"     // THERMAL_THROTTLE_TEMP_C, THERMAL_PAUSE_TEMP_C, THERMAL_RECOVER_MARGIN_C
#include "securacv_thermal.h"  // the provider: thermal_sensor_present(), thermal_read_die_c()

enum ThermalState : uint8_t {
  THERMAL_NORMAL    = 0,
  THERMAL_THROTTLED = 1,
  THERMAL_PAUSED    = 2
};

#define THERMAL_FAIL_SAFE_COUNT    3     // consecutive read failures before fail-safe throttle

namespace camera_thermal {

struct Tracker {
  ThermalState state;
  int8_t die_temp_c;   // the last good reading; 0 until one
  uint8_t fail_count;  // consecutive misses, held at THERMAL_FAIL_SAFE_COUNT
};

inline Tracker initial() { return Tracker{THERMAL_NORMAL, 0, 0}; }

// What one check did, for the camera's log lines.
struct Step {
  ThermalState prev;  // the state before the check
  bool changed;       // the state moved
  bool fail_safe;     // this miss raised the fail-safe throttle (once per episode)
  bool recovered;     // a good read ended a fail-safe episode
};

// A stream starts from normal with no miss counted (resetMetrics()). The
// count used to outlive the stream that made it, so the fail-safe fired on
// the first stream after boot only, and a later stream on a failing sensor
// streamed unthrottled.
inline void reset(Tracker& t) {
  t.state = THERMAL_NORMAL;
  t.fail_count = 0;
}

// One reading from a chip that has a sensor: read_ok false is a miss, else
// temp_c is the die temperature in °C. One or two misses keep the last
// state (a transient lock timeout); the third fails safe to THROTTLED (the
// cost is frame rate only) and reports it once; the next good read resumes
// the hysteresis and reports the recovery.
inline Step on_read(Tracker& t, bool read_ok, float temp_c) {
  Step s{t.state, false, false, false};
  if (!read_ok) {
    if (t.fail_count < THERMAL_FAIL_SAFE_COUNT) {
      t.fail_count++;
      if (t.fail_count == THERMAL_FAIL_SAFE_COUNT) {
        s.fail_safe = true;
        if (t.state == THERMAL_NORMAL) t.state = THERMAL_THROTTLED;
      }
    }
  } else {
    s.recovered = t.fail_count >= THERMAL_FAIL_SAFE_COUNT;
    t.fail_count = 0;
    t.die_temp_c = (int8_t)temp_c;
    if (t.state == THERMAL_PAUSED) {
      if (t.die_temp_c < THERMAL_PAUSE_TEMP_C - THERMAL_RECOVER_MARGIN_C) {
        t.state = THERMAL_THROTTLED;
      }
    } else if (t.state == THERMAL_THROTTLED) {
      if (t.die_temp_c >= THERMAL_PAUSE_TEMP_C) {
        t.state = THERMAL_PAUSED;
      } else if (t.die_temp_c < THERMAL_THROTTLE_TEMP_C - THERMAL_RECOVER_MARGIN_C) {
        t.state = THERMAL_NORMAL;
      }
    } else {
      if (t.die_temp_c >= THERMAL_PAUSE_TEMP_C) {
        t.state = THERMAL_PAUSED;
      } else if (t.die_temp_c >= THERMAL_THROTTLE_TEMP_C) {
        t.state = THERMAL_THROTTLED;
      }
    }
  }
  s.changed = s.prev != t.state;
  return s;
}

// One check through a provider: present() first, and nothing is read, or
// missed, when it says the chip has no sensor.
template <typename Provider>
inline Step check(Tracker& t, const Provider& p) {
  if (!p.present()) return Step{t.state, false, false, false};
  float c = 0.0f;
  const bool ok = p.read(&c);
  return on_read(t, ok, c);
}

// Whether the camera's thermal protection is reading a sensor: false on a
// chip that has none, and false from the fail-safe miss until a good read.
inline bool sensor_ok(const Tracker& t, bool present) {
  return present && t.fail_count < THERMAL_FAIL_SAFE_COUNT;
}

// The frame delay a state paces at, from the one the user set.
inline uint32_t frame_delay_ms(ThermalState state, uint32_t user_ms) {
  if (state == THERMAL_PAUSED) return 500;
  if (state == THERMAL_THROTTLED) {
    uint32_t throttled = user_ms * 3;
    if (throttled < 120) throttled = 120;
    if (throttled > 500) throttled = 500;
    return throttled;
  }
  return user_ms;
}

// The camera's provider: the shared die-temperature provider.
struct DieSensor {
  bool present() const { return thermal_sensor_present(); }
  bool read(float* c) const { return thermal_read_die_c(c); }
};

}  // namespace camera_thermal

#endif  // SECURACV_CAMERA_THERMAL_H
