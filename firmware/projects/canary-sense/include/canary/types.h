#pragma once
#include <stdint.h>

// Everything the MQTT layer publishes about the radar witness, pre-coarsened
// at the privacy chokepoint: presence as a debounced state, occupants as a
// 0/1/2+ bucket, distance ONLY as a near/mid/far band (raw centimeters never
// leave main.cpp), and vitals — wellbeing builds only — as a binary lock
// plus P1-gated BPM numerics that never appear in events.
struct SenseSnapshot {
  const char* presence  = "unknown";  // "unknown" / "clear" / "present"
  bool        present   = false;      // presence == "present"
  const char* occupants = "0";        // "0" / "1" / "2+"
  const char* range     = "unknown";  // "unknown" / "near" / "mid" / "far"

  // Radar-link health (design doc §6: HEALTH_CAT_SENSOR territory).
  bool     radar_ok     = false;      // false while the UART is stalled
  uint32_t frame_errors = 0;          // checksum/oversize drops (monotonic)

  // Ambient light (BH1750); negative = sensor absent.
  float lux = -1.0f;

  const char* last_event = "boot";
  uint32_t uptime_s = 0;
  uint32_t ts_ms    = 0;

#ifdef CANARY_SENSE_VITALS
  // Wellbeing channel (never sealed-logged, never in events).
  bool     breathing_locked = false;  // P0 binary lock
  bool     bpm_valid        = false;  // true only while locked with sane values
  uint16_t breath_bpm       = 0;      // P1 numeric
  uint16_t heart_bpm        = 0;      // P1 numeric
#endif
};
