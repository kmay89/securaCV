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

// The cell under a point, clamped to the grid. The point is a box's center,
// and the box is whatever the sensor (or the Lab's sandbox) hands over, so
// the center and the products below are taken in int64_t, where no int box
// can overflow them (sweep A42): x + w/2 overflows an int for a box near the
// int range's ends, and px * cols for a center past INT_MAX / cols. A signed
// overflow is undefined, and the ESP32's gcc, the emulator's wasm32 clang and
// a host compiler resolved it differently, so the same box landed in
// different cells. A box whose int math did not overflow lands where it did.
inline void point_to_cell(int64_t px, int64_t py, int rows, int cols, int& r, int& c) {
  const int64_t safe_cols = (cols <= 0) ? 1 : cols;
  const int64_t safe_rows = (rows <= 0) ? 1 : rows;
  const int64_t col = (px * safe_cols) / FRAME_W;
  const int64_t row = (py * safe_rows) / FRAME_H;
  c = (int)(col < 0 ? 0 : (col > safe_cols - 1 ? safe_cols - 1 : col));
  r = (int)(row < 0 ? 0 : (row > safe_rows - 1 ? safe_rows - 1 : row));
}

// A box's area, in int64_t: w * h overflows an int, and a 32-bit long (the
// ESP32's and wasm32's; a 64-bit host's long held it, so the builds read the
// same box's proximity differently), once w * h passes INT32_MAX (sweep A42).
inline int64_t area_of(int w, int h) { return (int64_t)w * h; }

inline void bbox_to_voxel(const BBox& box, Voxel& voxel) {
  const int cols = (VOXEL_COLS == 0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS == 0) ? 1 : VOXEL_ROWS;
  int row = 0;
  int col = 0;
  point_to_cell((int64_t)box.x + (box.w / 2), (int64_t)box.y + (box.h / 2), rows, cols, row, col);
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
    point_to_cell((int64_t)box.x + (box.w / 2), (int64_t)box.y + (box.h / 2), rows, cols, row, col);
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
        area_of(best.w, best.h), area_of(FRAME_W, FRAME_H));
  }
  return out;
}

}  // namespace canary::vision::detection
