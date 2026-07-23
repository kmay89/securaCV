#pragma once

#include <stdint.h>

#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/types.h"
#include "canary/vision/optical_features.h"

// Pure, allocation-free detection pipeline shared by the ESP32 build and the
// browser firmware emulator. The camera/SSCMA transport is the silicon
// boundary: it supplies a container whose items have x/y/w/h/score/target.
// Everything after that boundary (class filter, threshold, primary box,
// occupancy, voxel, posture and proximity) is compiled from this one header.
namespace canary::vision::detection {

inline void point_to_cell(int px, int py, int rows, int cols, int& r, int& c) {
  const int safe_cols = (cols <= 0) ? 1 : cols;
  const int safe_rows = (rows <= 0) ? 1 : rows;
  c = (px * safe_cols) / FRAME_W;
  r = (py * safe_rows) / FRAME_H;
  if (c < 0) c = 0;
  if (c > (safe_cols - 1)) c = safe_cols - 1;
  if (r < 0) r = 0;
  if (r > (safe_rows - 1)) r = safe_rows - 1;
}

inline void bbox_to_voxel(const BBox& box, Voxel& voxel) {
  const int cols = (VOXEL_COLS == 0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS == 0) ? 1 : VOXEL_ROWS;
  int row = 0;
  int col = 0;
  point_to_cell(box.x + (box.w / 2), box.y + (box.h / 2), rows, cols, row, col);
  voxel.cols = (uint8_t)cols;
  voxel.rows = (uint8_t)rows;
  voxel.c = col;
  voxel.r = row;
}

template <typename Boxes>
VisionSample sample_from_boxes(const Boxes& boxes, const canary::cfg::DetectConfig& det) {
  const int cols = (VOXEL_COLS == 0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS == 0) ? 1 : VOXEL_ROWS;

  BBox best{};
  bool found = false;
  int best_score = -1;
  uint8_t count = 0;
  uint16_t mask = 0;

  for (int i = 0; i < (int)boxes.size(); ++i) {
    const auto& box = boxes[i];
    if (box.target != det.person_target) continue;
    if (box.score < det.score_min) continue;

    if (count < 255) ++count;

    int row = 0;
    int col = 0;
    point_to_cell(box.x + (box.w / 2), box.y + (box.h / 2), rows, cols, row, col);
    const int bit = row * cols + col;
    if (bit >= 0 && bit < 16) mask |= (uint16_t)(1u << bit);

    if (box.score > best_score) {
      best_score = box.score;
      best.x = box.x;
      best.y = box.y;
      best.w = box.w;
      best.h = box.h;
      best.score = box.score;
      found = true;
    }
  }

  VisionSample out{};
  out.person_now = found;
  out.bbox = found ? best : BBox{};
  out.voxel = Voxel{-1, -1, VOXEL_ROWS, VOXEL_COLS};
  out.person_count = count;
  out.voxel_mask = mask;
  out.posture = Posture::Unknown;
  out.proximity = Proximity::Unknown;

  if (found) {
    bbox_to_voxel(best, out.voxel);
    out.posture = canary::vision::optical::classify_posture(best.w, best.h);
    out.proximity = canary::vision::optical::classify_proximity(
        (long)best.w * best.h, (long)FRAME_W * FRAME_H);
  }
  return out;
}

}  // namespace canary::vision::detection
