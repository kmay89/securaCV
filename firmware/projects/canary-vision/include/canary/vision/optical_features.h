#pragma once
#include <stdint.h>
#include "canary/types.h"

// Coarse optical feature extraction — the invariant-safe coarsening applied to
// the person-detection model's raw box geometry.
//
// The Grove Vision AI V2 hands the host an array of boxes {class, score, x, y,
// w, h} per frame. This header turns that geometry into a handful of COARSE,
// non-identifying ordinals — a posture class, a proximity class, an occupancy
// count, an occupied-cell mask — and nothing finer. It deliberately emits no
// coordinates, no aspect angle, no area, no distance, and no per-person tokens:
//
//   - Invariant II (No Identity Substrate): ordinals cannot fingerprint a body;
//     aspect ratio and area are not biometrics and are not stable across people.
//   - Invariant E (Physics, Not Politics): the vocabulary is physical
//     ("horizontal", "near"), never interpretive ("collapsed", "intruder").
//   - Invariant I is not even in play: a bbox is already non-reversible to
//     pixels — this is coarser still.
//
// The logic is pure and Arduino-free so it is exercised host-side in
// firmware/tests_host/test_optical_features.cpp with -Wall -Wextra -Werror.
// vision_mgr.cpp reads the SSCMA boxes and calls these classifiers; it is the
// only place raw box geometry is handled, and none of it is published or sealed.

namespace canary::vision::optical {

// -------------------- Tunable thresholds (compile-time seeds) --------------------
// First-pass seeds, integer-only (no floats on the box). These are proxies, not
// measurements: posture from a bbox is coarser than pose, so both classes are
// gated wide and "ambiguous" is the honest middle (sitting/crouching/bending).
// A later pass can make these NVS-backed like the person-detect knobs (#788).

// Posture: compare box height h against width w (percent ratios, x100).
#ifndef OPT_POSTURE_UPRIGHT_RATIO_X100
#define OPT_POSTURE_UPRIGHT_RATIO_X100 130  // h >= 1.30*w  -> Upright (tall box)
#endif
#ifndef OPT_POSTURE_HORIZONTAL_RATIO_X100
#define OPT_POSTURE_HORIZONTAL_RATIO_X100 110  // w >= 1.10*h -> Horizontal (wide box)
#endif

// Proximity: box area as a percentage of the frame area.
#ifndef OPT_PROXIMITY_NEAR_PCT
#define OPT_PROXIMITY_NEAR_PCT 25  // >= 25% of frame -> Near
#endif
#ifndef OPT_PROXIMITY_FAR_PCT
#define OPT_PROXIMITY_FAR_PCT 6   // <= 6% of frame  -> Far  (else Mid)
#endif

// classify_proximity counts a box past the frame as 100% of it.
static_assert(OPT_PROXIMITY_NEAR_PCT <= 100, "a box covering the whole frame must read near");

// -------------------- Classifiers (pure) --------------------

// Posture from box width/height. Non-positive dims -> Unknown.
inline Posture classify_posture(int w, int h) {
  if (w <= 0 || h <= 0) return Posture::Unknown;
  // h*100 >= w*RATIO  ==  h/w >= RATIO/100, in int64_t: a 32-bit long
  // (wasm32's) overflowed once a side passed INT32_MAX / 130 (sweep A42),
  // which only the Lab's int boxes reach (the device's sides are uint16);
  // every positive int side fits here.
  if ((int64_t)h * 100 >= (int64_t)w * OPT_POSTURE_UPRIGHT_RATIO_X100) return Posture::Upright;
  if ((int64_t)w * 100 >= (int64_t)h * OPT_POSTURE_HORIZONTAL_RATIO_X100) return Posture::Horizontal;
  return Posture::Ambiguous;
}

// Proximity from box area vs frame area. Non-positive frame -> Unknown.
// The areas are int64_t (sweep A42): a 32-bit long (the ESP32's and
// wasm32's) overflowed in the caller's w * h past INT32_MAX and here in
// box_area * 100 past about 21.5 million, so the wasm32 emulator read an
// out-of-range box's proximity differently from a 64-bit host, whose long
// overflowed only once box_area * 100 passed INT64_MAX. The device's uint16
// boxes could reach both, though a 240x240 model does not return one.
inline Proximity classify_proximity(int64_t box_area, int64_t frame_area) {
  if (frame_area <= 0 || box_area <= 0) return Proximity::Unknown;
  // A box covers at most the whole frame: an area past the frame's counts
  // as 100%, which bounds box_area * 100 by frame_area * 100.
  if (box_area > frame_area) box_area = frame_area;
  // pct = box_area*100 / frame_area, computed without division-by-zero risk;
  // a frame too large to multiply by 100 is scaled down instead.
  const int64_t pct = (frame_area > INT64_MAX / 100) ? box_area / (frame_area / 100)
                                                      : (box_area * 100) / frame_area;
  if (pct >= OPT_PROXIMITY_NEAR_PCT) return Proximity::Near;
  if (pct <= OPT_PROXIMITY_FAR_PCT)  return Proximity::Far;
  return Proximity::Mid;
}

// -------------------- Names (for live-tier JSON / HA) --------------------

inline const char* posture_name(Posture p) {
  switch (p) {
    case Posture::Upright:    return "upright";
    case Posture::Ambiguous:  return "ambiguous";
    case Posture::Horizontal: return "horizontal";
    default:                  return "unknown";
  }
}

inline const char* proximity_name(Proximity p) {
  switch (p) {
    case Proximity::Far:  return "far";
    case Proximity::Mid:  return "mid";
    case Proximity::Near: return "near";
    default:              return "unknown";
  }
}

// Occupancy is intentionally COARSE — a bucket, never a running exact tally, so
// it cannot become a per-household occupancy history (free-signals §5, §7).
inline const char* occupancy_name(int count) {
  if (count <= 0) return "none";
  if (count == 1) return "one";
  if (count == 2) return "two";
  return "several";  // 3+
}

}  // namespace canary::vision::optical
