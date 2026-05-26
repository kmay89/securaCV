/*
 * SecuraCV Canary — 3-Layer Cascaded Vision Detection
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_vision.h"
#include <Arduino.h>
#include <string.h>
#include "canary_config.h"

#if FEATURE_VISION_DETECT

#include "securacv_camera.h"
#include "securacv_witness.h"
#include "esp_camera.h"

#if FEATURE_VISION_TFLITE
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "person_detect_model.h"
#endif

// ESP32 ROM JPEG decoder
extern "C" {
  #include "esp_jpg_decode.h"
}

namespace vision {

static bool s_initialized = false;
static bool s_running     = false;
static vision_config_t   s_cfg = VISION_CONFIG_DEFAULT;
static vision_event_cb_t s_cb  = nullptr;
static vision_stats_t    s_stats = {};

// Timing
static uint32_t s_last_process_ms   = 0;
static uint32_t s_motion_start_ms   = 0;
static uint32_t s_last_layer3_ms    = 0;

// Layer 1: JPEG size EMA
static float   s_jpeg_size_ema = 0.0f;
static bool    s_jpeg_baseline_set = false;
static const float JPEG_EMA_ALPHA = 0.1f;

// Layer 2: Block luminance baseline
static uint8_t s_block_baseline[VISION_GRID_TOTAL] = {};
static bool    s_block_baseline_set = false;
static const float BLOCK_EMA_ALPHA = 0.05f;

// Grayscale decode buffer (160x120 = 19200 bytes)
#define DECODE_W 160
#define DECODE_H 120
static uint8_t* s_gray_buf = nullptr;

// Motion state
static bool s_motion_active = false;

static inline uint8_t time_bucket_now() {
  return (uint8_t)((millis() / (10UL * 60UL * 1000UL)) % 144);
}

static void emit_event(vision_event_type_t type, uint8_t confidence, uint8_t zone) {
  if (!s_cb) return;
  vision_event_t evt;
  evt.event_type  = (uint8_t)type;
  evt.confidence  = confidence;
  evt.zone        = zone;
  evt.time_bucket = time_bucket_now();
  s_cb(&evt);
}

// ════════════════════════════════════════════════════════════════════════════
// JPEG DECODE CALLBACK (ESP32 ROM decoder)
// ════════════════════════════════════════════════════════════════════════════

struct JpgDecodeCtx {
  uint8_t* out;
  int      out_w;
  int      out_h;
};

static bool jpg_output_cb(void* arg, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h, uint8_t* data) {
  JpgDecodeCtx* ctx = (JpgDecodeCtx*)arg;
  for (uint16_t row = 0; row < h; row++) {
    int dst_y = y + row;
    if (dst_y >= ctx->out_h) break;
    for (uint16_t col = 0; col < w; col++) {
      int dst_x = x + col;
      if (dst_x >= ctx->out_w) continue;
      int src_idx = (row * w + col) * 3;
      uint8_t r = data[src_idx];
      uint8_t g = data[src_idx + 1];
      uint8_t b = data[src_idx + 2];
      ctx->out[dst_y * ctx->out_w + dst_x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
  }
  return true;
}

static bool decode_jpeg_to_gray(const uint8_t* jpg, size_t jpg_len,
                                uint8_t* gray, int w, int h) {
  JpgDecodeCtx ctx = { gray, w, h };
  esp_err_t err = esp_jpg_decode(jpg_len, JPG_SCALE_NONE,
                                 [](void*, uint8_t* src, size_t len, size_t* out) -> bool {
                                   *out = len;
                                   return true;
                                 },
                                 jpg_output_cb, &ctx);
  return err == ESP_OK;
}

// ════════════════════════════════════════════════════════════════════════════
// LAYER 1: JPEG SIZE DELTA
// ════════════════════════════════════════════════════════════════════════════

static bool layer1_check(size_t frame_bytes) {
  if (!s_jpeg_baseline_set) {
    s_jpeg_size_ema = (float)frame_bytes;
    s_jpeg_baseline_set = true;
    return false;
  }

  float current = (float)frame_bytes;
  float delta_pct = 0.0f;
  if (s_jpeg_size_ema > 100.0f) {
    delta_pct = fabsf(current - s_jpeg_size_ema) / s_jpeg_size_ema * 100.0f;
  }

  s_jpeg_size_ema = s_jpeg_size_ema * (1.0f - JPEG_EMA_ALPHA) + current * JPEG_EMA_ALPHA;

  return delta_pct >= (float)s_cfg.jpeg_delta_pct;
}

// ════════════════════════════════════════════════════════════════════════════
// LAYER 2: BLOCK LUMINANCE MOTION
// ════════════════════════════════════════════════════════════════════════════

struct MotionResult {
  bool   detected;
  uint8_t confidence;
  uint8_t zone;
  uint8_t changed_count;
};

static MotionResult layer2_check(const uint8_t* gray) {
  MotionResult result = { false, 0, 0, 0 };

  int block_w = DECODE_W / VISION_GRID_COLS;
  int block_h = DECODE_H / VISION_GRID_ROWS;

  uint8_t current_blocks[VISION_GRID_TOTAL];
  bool    changed[VISION_GRID_TOTAL];
  int     changed_count = 0;
  int     cx_sum = 0, cy_sum = 0;

  for (int by = 0; by < VISION_GRID_ROWS; by++) {
    for (int bx = 0; bx < VISION_GRID_COLS; bx++) {
      int idx = by * VISION_GRID_COLS + bx;
      uint32_t sum = 0;
      int count = 0;
      for (int py = by * block_h; py < (by + 1) * block_h && py < DECODE_H; py++) {
        for (int px = bx * block_w; px < (bx + 1) * block_w && px < DECODE_W; px++) {
          sum += gray[py * DECODE_W + px];
          count++;
        }
      }
      current_blocks[idx] = (uint8_t)(count > 0 ? sum / count : 0);

      if (!s_block_baseline_set) {
        changed[idx] = false;
      } else {
        int delta = abs((int)current_blocks[idx] - (int)s_block_baseline[idx]);
        changed[idx] = delta > s_cfg.luminance_threshold;
        if (changed[idx]) {
          changed_count++;
          cx_sum += bx;
          cy_sum += by;
        }
      }
    }
  }

  // Update baseline with EMA
  for (int i = 0; i < VISION_GRID_TOTAL; i++) {
    if (!s_block_baseline_set) {
      s_block_baseline[i] = current_blocks[i];
    } else {
      s_block_baseline[i] = (uint8_t)(
        (float)s_block_baseline[i] * (1.0f - BLOCK_EMA_ALPHA) +
        (float)current_blocks[i] * BLOCK_EMA_ALPHA);
    }
  }

  if (!s_block_baseline_set) {
    s_block_baseline_set = true;
    return result;
  }

  // Global illumination filter: if >80% of blocks changed, it's lighting
  if (changed_count > (VISION_GRID_TOTAL * 80 / 100)) {
    return result;
  }

  int threshold = VISION_GRID_TOTAL * s_cfg.block_change_pct / 100;
  if (threshold < 1) threshold = 1;

  if (changed_count >= threshold) {
    result.detected = true;
    result.changed_count = (uint8_t)changed_count;
    result.confidence = (uint8_t)((changed_count * 100) / VISION_GRID_TOTAL);
    if (result.confidence > 100) result.confidence = 100;
    // Zone from centroid of changed blocks (1-based)
    int cx = changed_count > 0 ? cx_sum / changed_count : 0;
    int cy = changed_count > 0 ? cy_sum / changed_count : 0;
    result.zone = (uint8_t)(cy * VISION_GRID_COLS + cx + 1);
  }

  return result;
}

// ════════════════════════════════════════════════════════════════════════════
// LAYER 3: TFLITE PERSON DETECTION (opt-in)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_VISION_TFLITE

static constexpr int kModelInputW = 96;
static constexpr int kModelInputH = 96;
static constexpr int kTensorArenaSize = 136 * 1024;

static uint8_t* s_tensor_arena = nullptr;
static tflite::MicroInterpreter* s_interpreter = nullptr;
static bool s_model_loaded = false;

static bool layer3_init() {
  s_tensor_arena = (uint8_t*)ps_malloc(kTensorArenaSize);
  if (!s_tensor_arena) {
    Serial.println("[VISION] Layer 3: PSRAM alloc failed for tensor arena");
    return false;
  }

  const tflite::Model* model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[VISION] Layer 3: model version mismatch");
    free(s_tensor_arena);
    s_tensor_arena = nullptr;
    return false;
  }

  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter interpreter(model, resolver,
    s_tensor_arena, kTensorArenaSize);

  if (interpreter.AllocateTensors() != kTfLiteOk) {
    Serial.println("[VISION] Layer 3: AllocateTensors failed");
    free(s_tensor_arena);
    s_tensor_arena = nullptr;
    return false;
  }

  s_interpreter = &interpreter;
  s_model_loaded = true;
  Serial.println("[VISION] Layer 3: TFLite person model loaded");
  return true;
}

static uint8_t layer3_classify(const uint8_t* gray_160x120) {
  if (!s_model_loaded || !s_interpreter) return 0;

  TfLiteTensor* input = s_interpreter->input(0);
  if (!input) return 0;

  // Bilinear downsample 160x120 → 96x96
  int8_t* input_data = input->data.int8;
  for (int y = 0; y < kModelInputH; y++) {
    int src_y = y * DECODE_H / kModelInputH;
    for (int x = 0; x < kModelInputW; x++) {
      int src_x = x * DECODE_W / kModelInputW;
      input_data[y * kModelInputW + x] = (int8_t)(gray_160x120[src_y * DECODE_W + src_x] - 128);
    }
  }

  if (s_interpreter->Invoke() != kTfLiteOk) return 0;

  TfLiteTensor* output = s_interpreter->output(0);
  if (!output) return 0;

  // Model outputs [not_person, person] — return person confidence 0-100
  int8_t person_score = output->data.int8[1];
  int confidence = ((int)person_score + 128) * 100 / 255;
  if (confidence < 0) confidence = 0;
  if (confidence > 100) confidence = 100;
  return (uint8_t)confidence;
}

#else /* !FEATURE_VISION_TFLITE */

static bool layer3_init() { return true; }
static uint8_t layer3_classify(const uint8_t*) { return 0; }

#endif /* FEATURE_VISION_TFLITE */

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

} // namespace vision

bool vision_init(const vision_config_t* cfg) {
  if (vision::s_initialized) return true;
  if (cfg) vision::s_cfg = *cfg;

  vision::s_gray_buf = (uint8_t*)ps_malloc(DECODE_W * DECODE_H);
  if (!vision::s_gray_buf) {
    Serial.println("[VISION] PSRAM alloc failed for grayscale buffer");
    return false;
  }

  if (!vision::layer3_init()) {
    Serial.println("[VISION] Layer 3 init failed (continuing without person detection)");
  }

  memset(&vision::s_stats, 0, sizeof(vision::s_stats));
  vision::s_jpeg_baseline_set = false;
  vision::s_block_baseline_set = false;
  vision::s_motion_active = false;
  vision::s_initialized = true;
  Serial.println("[VISION] Initialized — 3-layer cascade ready");
  return true;
}

void vision_deinit() {
  if (!vision::s_initialized) return;
  vision::s_running = false;
  if (vision::s_gray_buf) {
    free(vision::s_gray_buf);
    vision::s_gray_buf = nullptr;
  }
#if FEATURE_VISION_TFLITE
  if (vision::s_tensor_arena) {
    free(vision::s_tensor_arena);
    vision::s_tensor_arena = nullptr;
  }
  vision::s_interpreter = nullptr;
  vision::s_model_loaded = false;
#endif
  vision::s_initialized = false;
}

bool vision_start() {
  if (!vision::s_initialized) return false;
  vision::s_running = true;
  vision::s_last_process_ms = millis();
  Serial.println("[VISION] Detection started");
  return true;
}

void vision_stop() {
  if (vision::s_motion_active) {
    vision::s_motion_active = false;
    vision::emit_event(VISION_EVENT_MOTION_END, 0, 0);
  }
  vision::s_running = false;
}

bool vision_is_running() {
  return vision::s_running;
}

void vision_set_event_callback(vision_event_cb_t cb) {
  vision::s_cb = cb;
}

bool vision_process() {
  if (!vision::s_running) return false;

  uint32_t now = millis();
  if (now - vision::s_last_process_ms < vision::s_cfg.process_interval_ms) return false;
  vision::s_last_process_ms = now;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) return false;

  // Don't compete with active streaming for camera frames
  if (cam.isPeekActive()) return false;

  // Don't run during thermal throttle/pause
  if (cam.getThermalState() != THERMAL_NORMAL) return false;

  camera_fb_t* fb = cam.captureFrame();
  if (!fb) return false;

  vision::s_stats.frames_analyzed++;

  // ── Layer 1: JPEG size delta ──────────────────────────────────────
  bool l1_pass = vision::layer1_check(fb->len);
  if (!l1_pass) {
    cam.returnFrame(fb);
    // Check if motion should end (held for motion_hold_ms)
    if (vision::s_motion_active &&
        now - vision::s_motion_start_ms > vision::s_cfg.motion_hold_ms) {
      vision::s_motion_active = false;
      vision::s_stats.last_confidence = 0;
      vision::emit_event(VISION_EVENT_MOTION_END, 0, 0);
    }
    return false;
  }
  vision::s_stats.layer1_passes++;

  // ── Layer 2: Block luminance motion ───────────────────────────────
  bool decoded = vision::decode_jpeg_to_gray(fb->buf, fb->len,
                                              vision::s_gray_buf, DECODE_W, DECODE_H);
  cam.returnFrame(fb);

  if (!decoded) return false;

  vision::MotionResult motion = vision::layer2_check(vision::s_gray_buf);
  if (!motion.detected) {
    if (vision::s_motion_active &&
        now - vision::s_motion_start_ms > vision::s_cfg.motion_hold_ms) {
      vision::s_motion_active = false;
      vision::s_stats.last_confidence = 0;
      vision::emit_event(VISION_EVENT_MOTION_END, 0, 0);
    }
    return false;
  }
  vision::s_stats.layer2_passes++;

  // Fire motion event
  if (!vision::s_motion_active) {
    vision::s_motion_active = true;
    vision::s_stats.motion_events++;
    vision::emit_event(VISION_EVENT_MOTION, motion.confidence, motion.zone);
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR, "Vision: motion detected", nullptr);
  }
  vision::s_motion_start_ms = now;
  vision::s_stats.last_zone = motion.zone;
  vision::s_stats.last_confidence = motion.confidence;
  vision::s_stats.motion_active = true;

  // ── Layer 3: TFLite person detection (opt-in) ─────────────────────
#if FEATURE_VISION_TFLITE
  if (now - vision::s_last_layer3_ms >= vision::s_cfg.layer3_cooldown_ms) {
    uint8_t person_conf = vision::layer3_classify(vision::s_gray_buf);
    vision::s_stats.layer3_passes++;
    if (person_conf >= vision::s_cfg.person_confidence_min) {
      vision::s_stats.person_events++;
      vision::emit_event(VISION_EVENT_PERSON, person_conf, motion.zone);
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR, "Vision: person detected", nullptr);
    }
    vision::s_last_layer3_ms = now;
  }
#endif

  return true;
}

bool vision_get_stats(vision_stats_t* out) {
  if (!out) return false;
  *out = vision::s_stats;
  return true;
}

#else /* !FEATURE_VISION_DETECT */

bool vision_init(const vision_config_t*) { return false; }
void vision_deinit() {}
bool vision_start() { return false; }
void vision_stop() {}
bool vision_is_running() { return false; }
void vision_set_event_callback(vision_event_cb_t) {}
bool vision_process() { return false; }
bool vision_get_stats(vision_stats_t*) { return false; }

#endif /* FEATURE_VISION_DETECT */
