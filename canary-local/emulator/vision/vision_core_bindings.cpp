// Browser ABI for the real Canary Vision detection core.
//
// Emscripten links this boundary with the firmware's detection_pipeline.h,
// detect_config.cpp, presence_fsm.cpp and voxel_tracker.cpp. JavaScript may
// stage SSCMA boxes (the sensor boundary), but it cannot reproduce or override
// the firmware decisions made after those boxes arrive.

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>

#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/state/presence_fsm.h"
#include "canary/types.h"
#include "canary/version.h"
#include "canary/vision/detection_pipeline.h"
#include "canary/vision/optical_features.h"

namespace {
struct EmuBox {
  int x;
  int y;
  int w;
  int h;
  int score;
  int target;
};

struct EmuBoxes {
  static constexpr int MAX = 32;
  EmuBox values[MAX]{};
  int count = 0;
  int size() const { return count; }
  const EmuBox& operator[](int index) const { return values[index]; }
};

EmuBoxes g_boxes;
VisionSample g_sample{};
canary::state::PresenceFSM g_fsm;
StateSnapshot g_snapshot{};
EventMsg g_event{};
const char* g_last_event = "boot";
char g_json[1400];

const char* json_string_or_null(const char* value, char* out, size_t cap) {
  if (!value || !value[0]) {
    snprintf(out, cap, "null");
  } else {
    // Event/reason values are compile-time firmware literals containing only
    // [a-z_]. Keeping the encoder narrow makes the ABI allocation-free.
    snprintf(out, cap, "\"%s\"", value);
  }
  return out;
}
}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* vision_emu_contract_json() {
  const auto& cfg = canary::cfg::detect();
  snprintf(
      g_json, sizeof(g_json),
      "{\"schema\":\"securacv.canary-vision.core/v1\","
      "\"firmware\":\"%s\",\"person_target\":%u,\"score_min\":%u,"
      "\"lost_timeout_ms\":%lu,\"dwell_start_ms\":%lu,"
      "\"score_min_lo\":%u,\"score_min_hi\":%u,"
      "\"lost_ms_lo\":%lu,\"lost_ms_hi\":%lu,"
      "\"dwell_ms_lo\":%lu,\"dwell_ms_hi\":%lu,"
      "\"frame_w\":%d,\"frame_h\":%d,\"voxel_rows\":%u,"
      "\"voxel_cols\":%u,\"invoke_period_ms\":%lu}",
      CANARY_FW_VERSION, (unsigned)cfg.person_target, (unsigned)cfg.score_min,
      (unsigned long)cfg.lost_timeout_ms, (unsigned long)cfg.dwell_start_ms,
      (unsigned)canary::cfg::DETECT_SCORE_MIN_LO,
      (unsigned)canary::cfg::DETECT_SCORE_MIN_HI,
      (unsigned long)canary::cfg::DETECT_LOST_MS_LO,
      (unsigned long)canary::cfg::DETECT_LOST_MS_HI,
      (unsigned long)canary::cfg::DETECT_DWELL_MS_LO,
      (unsigned long)canary::cfg::DETECT_DWELL_MS_HI, FRAME_W, FRAME_H,
      (unsigned)VOXEL_ROWS, (unsigned)VOXEL_COLS,
      (unsigned long)INVOKE_PERIOD_MS);
  return g_json;
}

EMSCRIPTEN_KEEPALIVE void vision_emu_reset() {
  g_boxes.count = 0;
  g_sample = VisionSample{};
  g_event = EventMsg{};
  g_last_event = "boot";
  g_fsm.reset();
  g_snapshot = g_fsm.snapshot(0, g_last_event);
}

EMSCRIPTEN_KEEPALIVE void vision_emu_set_config(int target, int score,
                                                unsigned int lost_ms,
                                                unsigned int dwell_ms) {
  if (target < 0) target = 0;
  if (target > 255) target = 255;
  if (score < 0) score = 0;
  if (score > 255) score = 255;
  canary::cfg::detect_set_person_target((uint8_t)target);
  canary::cfg::detect_set_score_min((uint8_t)score);
  canary::cfg::detect_set_lost_timeout_ms((uint32_t)lost_ms);
  canary::cfg::detect_set_dwell_start_ms((uint32_t)dwell_ms);
}

EMSCRIPTEN_KEEPALIVE void vision_emu_begin_frame() { g_boxes.count = 0; }

EMSCRIPTEN_KEEPALIVE int vision_emu_push_box(int x, int y, int w, int h,
                                             int score, int target) {
  if (g_boxes.count >= EmuBoxes::MAX) return 0;
  g_boxes.values[g_boxes.count++] = EmuBox{x, y, w, h, score, target};
  return 1;
}

EMSCRIPTEN_KEEPALIVE const char* vision_emu_tick_json(unsigned int now_ms) {
  g_sample = canary::vision::detection::sample_from_boxes(
      g_boxes, canary::cfg::detect());
  g_event = EventMsg{};
  const bool emitted = g_fsm.tick(g_sample, (uint32_t)now_ms, g_event);
  if (emitted && g_event.event_name) g_last_event = g_event.event_name;
  g_snapshot = g_fsm.snapshot((uint32_t)now_ms, g_last_event);

  char bbox[180];
  if (g_sample.person_now) {
    snprintf(bbox, sizeof(bbox),
             "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"score\":%d}",
             g_sample.bbox.x, g_sample.bbox.y, g_sample.bbox.w,
             g_sample.bbox.h, g_sample.bbox.score);
  } else {
    snprintf(bbox, sizeof(bbox), "null");
  }
  char event_json[96];
  char reason_json[96];
  json_string_or_null(emitted ? g_event.event_name : nullptr,
                      event_json, sizeof(event_json));
  json_string_or_null(emitted ? g_event.reason : nullptr,
                      reason_json, sizeof(reason_json));

  snprintf(
      g_json, sizeof(g_json),
      "{\"sample\":{\"person_now\":%s,\"bbox\":%s,"
      "\"voxel\":{\"r\":%d,\"c\":%d,\"rows\":%u,\"cols\":%u},"
      "\"person_count\":%u,\"posture\":\"%s\","
      "\"proximity\":\"%s\",\"voxel_mask\":%u},"
      "\"fsm\":{\"presence\":%s,\"dwelling\":%s,"
      "\"confidence\":%d,\"presence_ms\":%lu,\"dwell_ms\":%lu},"
      "\"event\":%s,\"reason\":%s}",
      g_sample.person_now ? "true" : "false", bbox,
      g_sample.voxel.r, g_sample.voxel.c, (unsigned)g_sample.voxel.rows,
      (unsigned)g_sample.voxel.cols, (unsigned)g_sample.person_count,
      canary::vision::optical::posture_name(g_sample.posture),
      canary::vision::optical::proximity_name(g_sample.proximity),
      (unsigned)g_sample.voxel_mask,
      g_snapshot.presence ? "true" : "false",
      g_snapshot.dwelling ? "true" : "false", g_snapshot.confidence,
      (unsigned long)g_snapshot.presence_ms,
      (unsigned long)g_snapshot.dwell_ms, event_json, reason_json);
  return g_json;
}

}  // extern "C"
