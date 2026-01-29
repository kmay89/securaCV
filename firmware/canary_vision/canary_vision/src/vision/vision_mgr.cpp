#include "canary/vision/vision_mgr.h"
#include "canary/config.h"
#include "canary/log.h"

#include <Wire.h>
#include <Seeed_Arduino_SSCMA.h>

namespace canary::vision {

static SSCMA AI;
static bool g_inited=false;

static bool pick_best_person_box(BBox& out) {
  bool found=false;
  int best=-1;

  auto& boxes = AI.boxes();
  for (int i=0;i<boxes.size();i++) {
    const auto& b = boxes[i];
    if (b.target != PERSON_TARGET) continue;
    if (b.score < SCORE_MIN) continue;
    if (b.score > best) {
      best = b.score;
      out.x=b.x; out.y=b.y; out.w=b.w; out.h=b.h; out.score=b.score;
      found=true;
    }
  }
  return found;
}

static void bbox_to_voxel(const BBox& bb, Voxel& v) {
  const int cx = bb.x + (bb.w/2);
  const int cy = bb.y + (bb.h/2);

  const int cols = (VOXEL_COLS==0) ? 1 : VOXEL_COLS;
  const int rows = (VOXEL_ROWS==0) ? 1 : VOXEL_ROWS;

  int c = (cx * cols) / FRAME_W;
  int r = (cy * rows) / FRAME_H;

  if (c<0) c=0; if (c>(cols-1)) c=cols-1;
  if (r<0) r=0; if (r>(rows-1)) r=rows-1;

  v.cols = (uint8_t)cols;
  v.rows = (uint8_t)rows;
  v.c = c;
  v.r = r;
}

void init() {
  Wire.begin();
  AI.begin();
  delay(250);

  log_header("I2C");
  Serial.printf("Grove Vision AI ID=%d\n", (int)AI.ID());
  g_inited=true;
}

bool sample(VisionSample& out) {
  if (!g_inited) return false;

  const bool invokeOk = AI.invoke(1, false, false);
  const bool hasBoxes = (AI.boxes().size() > 0);
  if (!(invokeOk || hasBoxes)) return false;

  BBox bb{};
  const bool personNow = pick_best_person_box(bb);

  out.person_now = personNow;
  out.bbox = bb;
  out.voxel = Voxel{ -1, -1, VOXEL_ROWS, VOXEL_COLS };

  if (personNow) bbox_to_voxel(bb, out.voxel);
  return true;
}

} // namespace
