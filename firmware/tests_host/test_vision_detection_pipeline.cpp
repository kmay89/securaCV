// The browser Vision bench and the ESP32 build compile this exact pipeline.
// This hosted suite pins the SSCMA-box boundary without Arduino or hardware.

#include <cassert>
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

  std::puts("PASS test_vision_detection_pipeline");
  return 0;
}
