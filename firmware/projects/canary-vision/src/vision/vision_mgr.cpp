#include "canary/vision/vision_mgr.h"
#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/vision/optical_features.h"
#include "canary/log.h"

#include <Wire.h>
#include <Seeed_Arduino_SSCMA.h>

#include "pins.h"

namespace canary::vision {

static SSCMA AI;
static bool g_inited = false;

// Map a pixel point to its (rows x cols) grid cell, clamped to the grid.
static void point_to_cell(int px, int py, int rows, int cols, int& r, int& c) {
  const int C = (cols <= 0) ? 1 : cols;
  const int R = (rows <= 0) ? 1 : rows;
  c = (px * C) / FRAME_W;
  r = (py * R) / FRAME_H;
  if (c < 0) c = 0;
  if (c > (C - 1)) c = C - 1;
  if (r < 0) r = 0;
  if (r > (R - 1)) r = R - 1;
}

static void bbox_to_voxel(const BBox& bb, Voxel& v) {
  const int cols = (VOXEL_COLS == 0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS == 0) ? 1 : VOXEL_ROWS;
  int r, c;
  point_to_cell(bb.x + (bb.w / 2), bb.y + (bb.h / 2), rows, cols, r, c);
  v.cols = (uint8_t)cols;
  v.rows = (uint8_t)rows;
  v.c = c;
  v.r = r;
}

void init() {
  // Explicit pins from the board's pins.h: the Arduino variant defaults
  // differ per board (ESP32-C3 DevKit: 8/9, XIAO C3: 6/7, XIAO S3: 5/6)
  // and the DevKit default does NOT match our documented Grove wiring.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL);
  const bool ok = AI.begin();
  delay(250);

  log_header("I2C");
  if (!ok) {
    canary::dbg_serial().println("ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)");
  }
  canary::dbg_serial().printf("Grove Vision AI ID=%d\n", (int)AI.ID());

  g_inited = true;
}

bool sample(VisionSample& out) {
  if (!g_inited) return false;

  const bool invokeOk = AI.invoke(1, false, false);
  const bool hasBoxes = (AI.boxes().size() > 0);
  if (!(invokeOk || hasBoxes)) return false;

  // Runtime settings (NVS-backed, adjustable from HA) — the loaded SSCMA
  // model decides the class index and score calibration.
  const auto& det = canary::cfg::detect();

  const int cols = (VOXEL_COLS == 0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS == 0) ? 1 : VOXEL_ROWS;

  // Single pass over ALL boxes: the highest-score box is the primary subject
  // (what today's pipeline already surfaces), while every qualifying box also
  // feeds the coarse occupancy count and the occupied-cell mask. No extra
  // inference — the boxes are already in RAM from AI.invoke() above.
  BBox best{};
  bool found = false;
  int bestScore = -1;
  uint8_t count = 0;
  uint16_t mask = 0;

  auto& boxes = AI.boxes();
  for (int i = 0; i < boxes.size(); i++) {
    const auto& b = boxes[i];
    if (b.target != det.person_target) continue;
    if (b.score < det.score_min) continue;

    if (count < 255) count++;

    int r, c;
    point_to_cell(b.x + (b.w / 2), b.y + (b.h / 2), rows, cols, r, c);
    const int bit = r * cols + c;
    if (bit >= 0 && bit < 16) mask |= (uint16_t)(1u << bit);

    if (b.score > bestScore) {
      bestScore = b.score;
      best.x = b.x; best.y = b.y; best.w = b.w; best.h = b.h; best.score = b.score;
      found = true;
    }
  }

  out.person_now   = found;
  out.bbox         = found ? best : BBox{};
  out.voxel        = Voxel{ -1, -1, VOXEL_ROWS, VOXEL_COLS };
  out.person_count = count;
  out.voxel_mask   = mask;
  out.posture      = Posture::Unknown;
  out.proximity    = Proximity::Unknown;

  if (found) {
    bbox_to_voxel(best, out.voxel);
    // Coarse ordinals derived from the primary box geometry — no coordinates,
    // no aspect angle, no area leave this function.
    out.posture   = optical::classify_posture(best.w, best.h);
    out.proximity = optical::classify_proximity((long)best.w * best.h,
                                                 (long)FRAME_W * FRAME_H);
  }
  return true;
}

} // namespace canary::vision
