#pragma once
#include <stdint.h>

struct BBox {
  int x=0, y=0, w=0, h=0;
  int score=0; // 0-100
};

struct Voxel {
  int r;
  int c;
  uint8_t rows;
  uint8_t cols;

  // Default: invalid / unset
  constexpr Voxel() : r(-1), c(-1), rows(0), cols(0) {}

  // Explicit constructor (robust: no aggregate-init surprises)
  constexpr Voxel(int r_, int c_, uint8_t rows_, uint8_t cols_)
      : r(r_), c(c_), rows(rows_), cols(cols_) {}

  static constexpr Voxel Invalid() { return Voxel(); }

  constexpr bool valid() const { return (r >= 0) && (c >= 0) && (rows > 0) && (cols > 0); }
};



// Coarse optical posture, derived from the person bounding-box aspect ratio.
// A bbox is NOT a skeleton and NOT keypoints — it is already non-reversible to
// pixels, so this stays clear of Invariant I. Physics-only phrasing
// (Invariant E): "horizontal", never "collapsed".
enum class Posture : uint8_t { Unknown = 0, Upright, Ambiguous, Horizontal };

// Coarse optical proximity, derived from the person bounding-box area as a
// fraction of the frame. Ordinal only — never a distance in meters.
enum class Proximity : uint8_t { Unknown = 0, Far, Mid, Near };

struct VisionSample {
  bool person_now=false;
  BBox bbox;   // primary (highest-score) person box — what the aim/live tier shows
  Voxel voxel; // coarsened 3x3 cell of the primary box's center

  // Derived from ALL qualifying boxes this frame (coarse, non-identifying).
  // These populate the live/telemetry tier only; the signed witness record
  // is unchanged (occupants stays binary until a spec PR — Invariant VI).
  uint8_t   person_count = 0;                  // internal: coarsened to the occupancy bucket at publish; NEVER emitted raw
  Posture   posture      = Posture::Unknown;   // from the primary box aspect ratio
  Proximity proximity    = Proximity::Unknown; // from the primary box area fraction
  uint16_t  voxel_mask   = 0;                  // occupied 3x3 cells: bit (r*cols + c)
};

struct StateSnapshot {
  bool presence=false;
  bool dwelling=false;

  uint32_t presence_ms=0;
  uint32_t dwell_ms=0;
  // Duration of the last COMPLETED stay (presence start → leave). The
  // presence_ended / interaction_likely events fire after presence_ms has
  // reset to 0, so this is the number "the visit lasted 45 s" reads —
  // live/telemetry tier only, never part of the signed record.
  uint32_t visit_ms=0;

  int confidence=0; // percent
  Voxel voxel;
  BBox bbox;

  // Coarse optical extras (live/telemetry tier — not part of the signed record)
  uint8_t   person_count = 0;   // internal: coarsened to the occupancy bucket at publish; NEVER emitted raw
  Posture   posture      = Posture::Unknown;
  Proximity proximity    = Proximity::Unknown;
  uint16_t  voxel_mask   = 0;

  const char* last_event="boot";
  uint32_t uptime_s=0;
  uint32_t ts_ms=0;
};

struct EventMsg {
  const char* event_name=nullptr;
  const char* reason=nullptr;
};
