/**
 * @file test_optical_features.cpp
 * @brief Host-build unit tests for the coarse optical feature classifiers.
 *
 * Compiles Arduino-free (see the sibling Makefile):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I ../projects/canary-vision/include \
 *       test_optical_features.cpp -o build/test_optical_features && ./build/...
 *
 * These pin the invariant-safe coarsening applied to the person-detection
 * model's box geometry: bbox aspect ratio -> posture ordinal, bbox area
 * fraction -> proximity ordinal, count -> occupancy bucket. No coordinate,
 * angle, area, or distance is ever emitted — the tests assert only ordinals
 * and their names, which is exactly what the firmware is allowed to publish.
 */

#include "canary/vision/optical_features.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace canary::vision::optical;

namespace {

int g_checks = 0;
#define CHECK(cond) do { assert(cond); ++g_checks; } while (0)

// Posture: tall box -> Upright, wide box -> Horizontal, square-ish ->
// Ambiguous, degenerate -> Unknown. Boundaries are inclusive on the "class"
// side (>=), matching the integer ratio math in optical_features.h.
void test_posture() {
  CHECK(classify_posture(40, 120) == Posture::Upright);      // h/w = 3.0
  CHECK(classify_posture(100, 130) == Posture::Upright);     // exactly 1.30
  CHECK(classify_posture(100, 129) == Posture::Ambiguous);   // just under 1.30

  CHECK(classify_posture(120, 40) == Posture::Horizontal);   // w/h = 3.0
  CHECK(classify_posture(110, 100) == Posture::Horizontal);  // exactly 1.10
  CHECK(classify_posture(109, 100) == Posture::Ambiguous);   // just under 1.10

  CHECK(classify_posture(100, 100) == Posture::Ambiguous);   // square

  CHECK(classify_posture(0, 100) == Posture::Unknown);
  CHECK(classify_posture(100, 0) == Posture::Unknown);
  CHECK(classify_posture(-5, 10) == Posture::Unknown);
}

// Proximity: box area as a percentage of a 240x240 (=57600) frame.
void test_proximity() {
  const long frame = 240L * 240L;  // 57600
  CHECK(classify_proximity(20000, frame) == Proximity::Near);  // ~34%
  CHECK(classify_proximity(14400, frame) == Proximity::Near);  // exactly 25%
  CHECK(classify_proximity(8000, frame)  == Proximity::Mid);   // ~13%
  CHECK(classify_proximity(3456, frame)  == Proximity::Far);   // exactly 6%
  CHECK(classify_proximity(2000, frame)  == Proximity::Far);   // ~3%
  CHECK(classify_proximity(0, frame)     == Proximity::Unknown);
  CHECK(classify_proximity(10000, 0)     == Proximity::Unknown);
}

// Occupancy is a COARSE bucket, never an exact running tally.
void test_occupancy() {
  CHECK(strcmp(occupancy_name(-1), "none") == 0);
  CHECK(strcmp(occupancy_name(0),  "none") == 0);
  CHECK(strcmp(occupancy_name(1),  "one") == 0);
  CHECK(strcmp(occupancy_name(2),  "two") == 0);
  CHECK(strcmp(occupancy_name(3),  "several") == 0);
  CHECK(strcmp(occupancy_name(9),  "several") == 0);
}

void test_names() {
  CHECK(strcmp(posture_name(Posture::Upright), "upright") == 0);
  CHECK(strcmp(posture_name(Posture::Horizontal), "horizontal") == 0);
  CHECK(strcmp(posture_name(Posture::Ambiguous), "ambiguous") == 0);
  CHECK(strcmp(posture_name(Posture::Unknown), "unknown") == 0);
  CHECK(strcmp(proximity_name(Proximity::Near), "near") == 0);
  CHECK(strcmp(proximity_name(Proximity::Mid), "mid") == 0);
  CHECK(strcmp(proximity_name(Proximity::Far), "far") == 0);
  CHECK(strcmp(proximity_name(Proximity::Unknown), "unknown") == 0);
}

}  // namespace

int main() {
  test_posture();
  test_proximity();
  test_occupancy();
  test_names();
  printf("[optical_features] %d checks passed\n", g_checks);
  return 0;
}
