// canary-local/tests/native/wasm32/pipeline_probe.cpp — the Vision detection
// pipeline (firmware/projects/canary-vision's detection_pipeline.h, with the
// classifiers in optical_features.h) behind a C ABI small enough for a
// freestanding wasm32 build (sweep A49).
//
// ../wasm32.js builds this file twice from the same flags build.sh hands
// em++ for the Vision core: once with the host's clang for wasm32 and linked
// by wasm-ld (no libc, no C++ library: the two headers in stubs/ stand in for
// what the firmware headers include), and once with g++ for the host, with
// PIPELINE_PROBE_HOSTED defined so main() serves boxes over stdin and stdout.
// vision_wasm32.test.js then hands both builds the same boxes. The wasm32
// build has the 32-bit long the emulator's dist (and the ESP32) has; the g++
// build has the host's 64-bit one; so a product the pipeline takes in long
// reads the same box differently in the two, which no 64-bit build shows.
//
// This is not the dist and not emscripten's clang: it settles what the
// sources compute in a 32-bit long, not every difference between compilers.

#include <stdint.h>

#include "canary/vision/detection_pipeline.h"

namespace {

// The fields sample_from_boxes reads from an SSCMA box.
struct ProbeBox {
  int x;
  int y;
  int w;
  int h;
  int score;
  int target;
};

// One box, in the container shape sample_from_boxes takes.
struct OneBox {
  const ProbeBox* box;
  int size() const { return 1; }
  const ProbeBox& operator[](int) const { return *box; }
};

// One answer: every field of the VisionSample the pipeline returns.
enum { kPerson, kCount, kRow, kCol, kRows, kCols, kMask, kPosture, kProximity,
       kBoxX, kBoxY, kBoxW, kBoxH, kBoxScore, kFields };

int32_t g_answer[kFields];

}  // namespace

extern "C" {

int32_t probe_long_bits(void) { return (int32_t)(sizeof(long) * 8); }
int32_t probe_fields(void) { return kFields; }
int32_t probe_frame_w(void) { return FRAME_W; }
int32_t probe_frame_h(void) { return FRAME_H; }

// The pipeline's reading of one person box, scored 90 against a threshold of
// 70: the box arithmetic alone decides the cell, the posture and the proximity.
int32_t* probe_box(int32_t x, int32_t y, int32_t w, int32_t h) {
  canary::cfg::DetectConfig det{};
  det.person_target = 0;
  det.score_min = 70;
  const ProbeBox box{x, y, w, h, 90, 0};
  const VisionSample s = canary::vision::detection::sample_from_boxes(OneBox{&box}, det);
  g_answer[kPerson] = s.person_now ? 1 : 0;
  g_answer[kCount] = s.person_count;
  g_answer[kRow] = s.voxel.r;
  g_answer[kCol] = s.voxel.c;
  g_answer[kRows] = s.voxel.rows;
  g_answer[kCols] = s.voxel.cols;
  g_answer[kMask] = s.voxel_mask;
  g_answer[kPosture] = (int32_t)s.posture;
  g_answer[kProximity] = (int32_t)s.proximity;
  g_answer[kBoxX] = s.bbox.x;
  g_answer[kBoxY] = s.bbox.y;
  g_answer[kBoxW] = s.bbox.w;
  g_answer[kBoxH] = s.bbox.h;
  g_answer[kBoxScore] = s.bbox.score;
  return g_answer;
}

}  // extern "C"

#ifdef PIPELINE_PROBE_HOSTED
#include <cstdio>

// The g++ build: four int32s per box on stdin (x, y, w, h); on stdout, this
// build's long width, kFields and the frame's size, then kFields int32s per
// answer, in the host's byte order.
int main() {
  const int32_t head[4] = {probe_long_bits(), probe_fields(), probe_frame_w(), probe_frame_h()};
  if (std::fwrite(head, sizeof head[0], 4, stdout) != 4) return 2;
  int32_t q[4];
  while (std::fread(q, sizeof q[0], 4, stdin) == 4) {
    if (std::fwrite(probe_box(q[0], q[1], q[2], q[3]), sizeof(int32_t), kFields, stdout) != (size_t)kFields) return 2;
  }
  return std::ferror(stdin) ? 1 : 0;
}
#endif
