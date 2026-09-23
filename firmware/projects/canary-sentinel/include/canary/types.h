#pragma once
#include <stdint.h>

// Everything the MQTT layer publishes about the guardian, pre-coarsened at the
// privacy chokepoint (main.cpp emit_claim, requirement R4): the
// securacv::fusion::FusionResult vocabulary as wire strings and small
// integers, nothing finer. No centimeters, no MACs, no per-target track, no
// imagery — those are read, turned into a Vote, and dropped in main.cpp.

// One fused claim, stringified at the chokepoint: exactly the fields the
// `sentinel` v1 canonical signs (common/identity/device_signature.h), so what
// is published and what is signed cannot differ.
struct SentinelClaim {
  const char* event         = "level_changed";
  const char* level         = "clear";    // clear/aware/present/confirmed/loiter/anomaly
  uint8_t     confidence    = 0;          // 0..100 fused evidence score
  uint8_t     anomaly       = 0;          // 0..100 suspicion accumulator
  const char* occupancy     = "unknown";  // unknown / 0 / 1 / 2+
  const char* range         = "unknown";  // unknown / near / mid / far
  uint8_t     modality_bits = 0;          // securacv::fusion::Modality classes voting >= Weak
};

// The retained state snapshot (securacv/<id>/state): the latest claim plus
// the derived booleans HA's binary sensors read.
struct SentinelSnapshot {
  SentinelClaim claim;
  bool        present    = false;   // level is present / confirmed / loiter
  bool        anomalous  = false;   // level is anomaly (the overlay that latches)
  bool        denied_any = false;   // a channel is blinded / stalled right now (R2, R6)
  uint8_t     strong_modalities = 0;  // distinct classes voting Strong
  const char* modalities = "none";  // modality_bits as class names ("thermal,radar")
  const char* last_event = "boot";
  uint32_t    uptime_s   = 0;
  uint32_t    ts_ms      = 0;
};
