// The browser Vision bench and the ESP32 build compile this exact pipeline.
// This hosted suite pins the SSCMA-box boundary without Arduino or hardware.
//
// Pinned (sweep A42): a box anywhere in the int range lands in the cell, and
// reads the posture and proximity, that exact arithmetic says, in every
// build. The pipeline took the box's center (x + w/2) and the cell
// (px * cols / FRAME_W) in int and the area in long, so an out-of-range box
// overflowed: undefined behavior, which the ESP32's gcc, the emulator's
// wasm32 clang and a 64-bit host resolved differently (a box two billion
// pixels wide read proximity "unknown" on the wasm32 dist and "near" on a
// 64-bit host, and landed in different cells). The oracle below computes
// each answer in __int128, where nothing these ints make can overflow, and
// the Makefile also builds this suite under -fsanitize=undefined, so an
// overflow fails it on any host whatever value the compiler made of it.

#include <cassert>
#include <climits>
#include <cstdio>
#include <vector>

#include "canary/vision/detection_pipeline.h"

struct TestBox {
  int x;
  int y;
  int w;
  int h;
  int score;
  int target;
};

namespace {

using i128 = __int128;

// The cell under one coordinate of the center, as exact arithmetic says:
// C++ division truncates toward zero, then the grid clamps.
int oracle_cell(int at, int extent, int n, int frame) {
  const i128 center = (i128)at + extent / 2;
  i128 cell = center * n / frame;
  if (cell < 0) cell = 0;
  if (cell > n - 1) cell = n - 1;
  return (int)cell;
}

Posture oracle_posture(int w, int h) {
  if (w <= 0 || h <= 0) return Posture::Unknown;
  if ((i128)h * 100 >= (i128)w * OPT_POSTURE_UPRIGHT_RATIO_X100) return Posture::Upright;
  if ((i128)w * 100 >= (i128)h * OPT_POSTURE_HORIZONTAL_RATIO_X100) return Posture::Horizontal;
  return Posture::Ambiguous;
}

// Area as a whole-percent of the frame, uncapped: a box past the frame is
// over 100% and so near, whatever its size.
Proximity oracle_proximity(int w, int h) {
  const i128 area = (i128)w * h;
  if (area <= 0) return Proximity::Unknown;
  const i128 pct = area * 100 / ((i128)FRAME_W * FRAME_H);
  if (pct >= OPT_PROXIMITY_NEAR_PCT) return Proximity::Near;
  if (pct <= OPT_PROXIMITY_FAR_PCT) return Proximity::Far;
  return Proximity::Mid;
}

VisionSample one_box(const TestBox& b, const canary::cfg::DetectConfig& cfg) {
  const std::vector<TestBox> boxes = {b};
  return canary::vision::detection::sample_from_boxes(boxes, cfg);
}

void test_out_of_range_boxes_follow_exact_arithmetic(const canary::cfg::DetectConfig& cfg) {
  static const int V[] = {INT_MIN, INT_MIN / 2, -2000000000, -1000000, -100, -1, 0, 1, 40,
                          FRAME_W - 1, FRAME_W, 5000, 50000, 1000000, 1000000000,
                          2000000000, INT_MAX / 2, INT_MAX};
  long checked = 0;
  for (const int x : V)
    for (const int y : V)
      for (const int w : V)
        for (const int h : V) {
          const VisionSample s = one_box(TestBox{x, y, w, h, 90, 0}, cfg);
          const int c = oracle_cell(x, w, VOXEL_COLS, FRAME_W);
          const int r = oracle_cell(y, h, VOXEL_ROWS, FRAME_H);
          if (!s.person_now || s.voxel.c != c || s.voxel.r != r ||
              s.voxel_mask != (uint16_t)(1u << (r * VOXEL_COLS + c)) ||
              s.posture != oracle_posture(w, h) || s.proximity != oracle_proximity(w, h)) {
            std::fprintf(stderr,
                         "box x=%d y=%d w=%d h=%d: cell (%d,%d) mask %u posture %d proximity %d, "
                         "exact arithmetic says (%d,%d) mask %u posture %d proximity %d\n",
                         x, y, w, h, s.voxel.r, s.voxel.c, (unsigned)s.voxel_mask, (int)s.posture,
                         (int)s.proximity, r, c, 1u << (r * VOXEL_COLS + c),
                         (int)oracle_posture(w, h), (int)oracle_proximity(w, h));
            assert(false && "an out-of-range box left exact arithmetic");
          }
          checked++;
        }
  // The sweep item's box: two billion pixels wide, from the frame's corner.
  // Its center is a billion pixels right of the frame: the last column; its
  // area is far past the frame's: near (the wasm32 dist read unknown).
  const VisionSample wide = one_box(TestBox{0, 0, 2000000000, 70, 90, 0}, cfg);
  assert(wide.voxel.c == VOXEL_COLS - 1 && wide.voxel.r == 0);
  assert(wide.proximity == Proximity::Near && wide.posture == Posture::Horizontal);
  // A center past INT_MAX (x + w/2 overflowed an int): the last cell, not
  // the first, where the wrapped negative sum used to land it.
  const VisionSample past = one_box(TestBox{2000000000, 2000000000, 1000000000, 1000000000, 90, 0}, cfg);
  assert(past.voxel.c == VOXEL_COLS - 1 && past.voxel.r == VOXEL_ROWS - 1);
  std::printf("  %ld out-of-range boxes follow exact arithmetic\n", checked);
}

}  // namespace

int main() {
  canary::cfg::DetectConfig cfg{};
  cfg.person_target = 0;
  cfg.score_min = 70;
  cfg.lost_timeout_ms = 1500;
  cfg.dwell_start_ms = 10000;

  const std::vector<TestBox> boxes = {
      {10, 10, 20, 20, 99, 8},   // high-scoring non-person
      {20, 20, 20, 20, 69, 0},   // person below the firmware threshold
      {70, 70, 40, 80, 70, 0},   // exactly at threshold
      {110, 90, 70, 140, 92, 0}, // primary subject
  };
  const VisionSample sample =
      canary::vision::detection::sample_from_boxes(boxes, cfg);

  assert(sample.person_now);
  assert(sample.bbox.score == 92);
  assert(sample.person_count == 2);
  assert(sample.voxel.valid());
  assert(sample.voxel.rows == VOXEL_ROWS);
  assert(sample.voxel.cols == VOXEL_COLS);
  assert(sample.voxel_mask != 0);
  assert(sample.posture == Posture::Upright);
  assert(sample.proximity == Proximity::Mid);

  cfg.score_min = 100;
  const VisionSample empty =
      canary::vision::detection::sample_from_boxes(boxes, cfg);
  assert(!empty.person_now);
  assert(empty.person_count == 0);
  assert(!empty.voxel.valid());
  assert(empty.voxel_mask == 0);

  cfg.score_min = 70;
  test_out_of_range_boxes_follow_exact_arithmetic(cfg);

  std::puts("PASS test_vision_detection_pipeline");
  return 0;
}
