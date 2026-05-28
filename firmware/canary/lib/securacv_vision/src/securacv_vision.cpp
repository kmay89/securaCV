/*
 * SecuraCV Canary — 3-Layer Cascaded Vision Detection
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_vision.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "canary_config.h"

#if FEATURE_VISION_DETECT

#include "securacv_camera.h"
#include "securacv_witness.h"
#include "esp_camera.h"
#include "img_converters.h"
#include <Preferences.h>

#if FEATURE_VISION_TFLITE
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "person_detect_model.h"
#endif

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

// Thermal duty cycle — alternate between active and rest periods
static uint32_t s_duty_cycle_start  = 0;
static bool     s_duty_resting      = false;

// Sustained activity backoff
static uint8_t  s_consecutive_l2    = 0;

// Scene tamper detection — sustained global change counter
static uint8_t  s_tamper_counter = 0;
static bool     s_tamper_fired   = false;

// Layer 1: JPEG size EMA + adaptive variance
static float   s_jpeg_size_ema = 0.0f;
static float   s_jpeg_var_ema  = 0.0f;
static bool    s_jpeg_baseline_set = false;
static const float JPEG_EMA_ALPHA = 0.1f;

// Layer 2: Block luminance baseline + adaptive variance
static uint8_t s_block_baseline[VISION_GRID_TOTAL] = {};
static float   s_block_var[VISION_GRID_TOTAL] = {};
static bool    s_block_baseline_set = false;
static const float BLOCK_EMA_ALPHA = 0.05f;
static const float VAR_EMA_ALPHA   = 0.02f;
static const float MIN_STDDEV      = 5.0f;

// Object removal: long-term stable baseline (very slow EMA)
static float   s_longterm_baseline[VISION_GRID_TOTAL] = {};
static bool    s_longterm_set = false;
static const float LONGTERM_EMA_ALPHA = 0.001f;
static const float OBJ_DIVERGE_THRESH = 25.0f;
static const float OBJ_REVERT_THRESH  = 8.0f;
static uint8_t s_obj_present[VISION_GRID_TOTAL] = {};

// Grayscale decode buffer (160x120 = 19200 bytes)
#define DECODE_W 160
#define DECODE_H 120
static uint8_t* s_gray_buf = nullptr;

// Thumbnail snapshot (40x30, updated after each decode)
static uint8_t s_thumb[VISION_THUMB_W * VISION_THUMB_H] = {};
static bool s_thumb_valid = false;

static void update_thumbnail() {
  if (!s_gray_buf) return;
  const int sx = DECODE_W / VISION_THUMB_W;
  const int sy = DECODE_H / VISION_THUMB_H;
  for (int y = 0; y < VISION_THUMB_H; y++) {
    for (int x = 0; x < VISION_THUMB_W; x++) {
      uint32_t sum = 0;
      for (int dy = 0; dy < sy; dy++)
        for (int dx = 0; dx < sx; dx++)
          sum += s_gray_buf[(y * sy + dy) * DECODE_W + x * sx + dx];
      s_thumb[y * VISION_THUMB_W + x] = (uint8_t)(sum / (sx * sy));
    }
  }
  s_thumb_valid = true;
}

// Event history ring buffer
static vision_history_entry_t s_history[VISION_HISTORY_SIZE] = {};
static int s_history_head = 0;
static int s_history_count = 0;
static portMUX_TYPE s_history_mux = portMUX_INITIALIZER_UNLOCKED;

static void history_push(uint8_t type, uint8_t confidence, uint8_t zone) {
  portENTER_CRITICAL(&s_history_mux);
  s_history[s_history_head] = { type, confidence, zone, millis() };
  s_history_head = (s_history_head + 1) % VISION_HISTORY_SIZE;
  if (s_history_count < VISION_HISTORY_SIZE) s_history_count++;
  portEXIT_CRITICAL(&s_history_mux);
}

// Motion state
static bool s_motion_active = false;

static inline uint8_t time_bucket_now() {
  return (uint8_t)((millis() / (10UL * 60UL * 1000UL)) % 144);
}

static void emit_event(vision_event_type_t type, uint8_t confidence, uint8_t zone) {
  history_push((uint8_t)type, confidence, zone);
  if (!s_cb) return;
  vision_event_t evt;
  evt.event_type  = (uint8_t)type;
  evt.confidence  = confidence;
  evt.zone        = zone;
  evt.time_bucket = time_bucket_now();
  s_cb(&evt);
}

// ════════════════════════════════════════════════════════════════════════════
// JPEG → GRAYSCALE DECODE
// ════════════════════════════════════════════════════════════════════════════

// RGB888 temporary buffer — allocated for max expected frame (VGA).
// Larger frames (XGA+) are skipped to avoid excessive PSRAM use.
static uint8_t* s_rgb_buf = nullptr;
#define RGB_BUF_MAX_PIXELS (640 * 480)
#define RGB_BUF_SIZE (RGB_BUF_MAX_PIXELS * 3)

static bool decode_and_downsample(camera_fb_t* fb, uint8_t* gray,
                                  int out_w, int out_h) {
  int src_w = fb->width;
  int src_h = fb->height;
  if ((size_t)(src_w * src_h) > RGB_BUF_MAX_PIXELS) return false;

  if (!s_rgb_buf) {
    s_rgb_buf = (uint8_t*)ps_malloc(RGB_BUF_SIZE);
    if (!s_rgb_buf) return false;
  }
  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, s_rgb_buf)) return false;

  // Nearest-neighbor downsample to out_w x out_h grayscale
  for (int y = 0; y < out_h; y++) {
    int sy = y * src_h / out_h;
    for (int x = 0; x < out_w; x++) {
      int sx = x * src_w / out_w;
      int si = (sy * src_w + sx) * 3;
      gray[y * out_w + x] = (uint8_t)((s_rgb_buf[si] * 77 + s_rgb_buf[si+1] * 150 + s_rgb_buf[si+2] * 29) >> 8);
    }
  }
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// LAYER 1: JPEG SIZE DELTA
// ════════════════════════════════════════════════════════════════════════════

static bool layer1_check(size_t frame_bytes) {
  if (!s_jpeg_baseline_set) {
    s_jpeg_size_ema = (float)frame_bytes;
    s_jpeg_var_ema  = 0.0f;
    s_jpeg_baseline_set = true;
    return false;
  }

  float current = (float)frame_bytes;
  float delta = fabsf(current - s_jpeg_size_ema);

  // Update variance EMA: tracks squared deviation from mean
  float sq_dev = (current - s_jpeg_size_ema) * (current - s_jpeg_size_ema);
  s_jpeg_var_ema = s_jpeg_var_ema * (1.0f - VAR_EMA_ALPHA) + sq_dev * VAR_EMA_ALPHA;

  s_jpeg_size_ema = s_jpeg_size_ema * (1.0f - JPEG_EMA_ALPHA) + current * JPEG_EMA_ALPHA;

  if (s_cfg.adaptive_enabled && s_jpeg_size_ema > 100.0f) {
    float stddev = sqrtf(s_jpeg_var_ema);
    if (stddev < MIN_STDDEV) stddev = MIN_STDDEV;
    float k = (float)s_cfg.jpeg_delta_pct / 10.0f;
    return delta > k * stddev;
  }

  float delta_pct = 0.0f;
  if (s_jpeg_size_ema > 100.0f) {
    delta_pct = delta / s_jpeg_size_ema * 100.0f;
  }
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
  bool    obj_removed;
  uint8_t obj_removed_zone;
};

static MotionResult layer2_check(const uint8_t* gray) {
  MotionResult result = { false, 0, 0, 0, false, 0 };

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
        s_stats.block_intensity[idx] = 0;
      } else {
        int delta = abs((int)current_blocks[idx] - (int)s_block_baseline[idx]);
        uint8_t fresh = (uint8_t)(delta > 255 ? 255 : delta);
        uint8_t decayed = (uint8_t)(s_stats.block_intensity[idx] * 3 / 4);
        s_stats.block_intensity[idx] = fresh > decayed ? fresh : decayed;

        int base_thresh = s_cfg.zone_sensitivity[idx] > 0
                          ? (int)s_cfg.zone_sensitivity[idx]
                          : (int)s_cfg.luminance_threshold;
        int thresh = base_thresh;
        if (s_cfg.adaptive_enabled) {
          float stddev = sqrtf(s_block_var[idx]);
          if (stddev < MIN_STDDEV) stddev = MIN_STDDEV;
          float k = (float)base_thresh / 10.0f;
          thresh = (int)(k * stddev);
          if (thresh < 3) thresh = 3;
        }
        changed[idx] = delta > thresh;

        if (changed[idx] && !(s_cfg.zone_mask[idx / 8] & (1 << (idx % 8)))) {
          changed[idx] = false;
        }
        if (changed[idx]) {
          changed_count++;
          cx_sum += bx;
          cy_sum += by;
        }
      }
    }
  }

  // Update baseline with EMA + per-block variance tracking
  int obj_removed_count = 0;
  int obj_cx = 0, obj_cy = 0;
  for (int i = 0; i < VISION_GRID_TOTAL; i++) {
    float cur = (float)current_blocks[i];
    if (!s_block_baseline_set) {
      s_block_baseline[i] = current_blocks[i];
      s_block_var[i] = 0.0f;
    } else {
      float diff = cur - (float)s_block_baseline[i];
      s_block_var[i] = s_block_var[i] * (1.0f - VAR_EMA_ALPHA) + diff * diff * VAR_EMA_ALPHA;
      s_block_baseline[i] = (uint8_t)(
        (float)s_block_baseline[i] * (1.0f - BLOCK_EMA_ALPHA) + cur * BLOCK_EMA_ALPHA);
    }

    // Long-term baseline for object removal detection
    bool zone_enabled = (s_cfg.zone_mask[i / 8] >> (i % 8)) & 1;
    if (!s_longterm_set) {
      s_longterm_baseline[i] = cur;
      s_obj_present[i] = 0;
    } else if (!zone_enabled) {
      // Disabled zone: keep long-term baseline updated but skip removal tracking
      s_longterm_baseline[i] = s_longterm_baseline[i] * (1.0f - LONGTERM_EMA_ALPHA)
                               + cur * LONGTERM_EMA_ALPHA;
      s_obj_present[i] = 0;
    } else {
      float lt_delta = fabsf(cur - s_longterm_baseline[i]);
      if (s_obj_present[i] == 0 && lt_delta > OBJ_DIVERGE_THRESH) {
        s_obj_present[i] = 1;
      } else if (s_obj_present[i] == 1 && lt_delta < OBJ_REVERT_THRESH) {
        s_obj_present[i] = 0;
        obj_removed_count++;
        obj_cx += i % VISION_GRID_COLS;
        obj_cy += i / VISION_GRID_COLS;
      }
      // Only update long-term baseline when block is stable
      if (s_obj_present[i] == 0) {
        s_longterm_baseline[i] = s_longterm_baseline[i] * (1.0f - LONGTERM_EMA_ALPHA)
                                 + cur * LONGTERM_EMA_ALPHA;
      }
    }
  }

  if (!s_block_baseline_set) {
    s_block_baseline_set = true;
    s_longterm_set = true;
    return result;
  }

  // Object removal: multiple blocks reverted simultaneously
  if (obj_removed_count >= 3) {
    result.obj_removed = true;
    result.obj_removed_zone = (uint8_t)(
      (obj_cy / obj_removed_count) * VISION_GRID_COLS +
      (obj_cx / obj_removed_count) + 1);
  }

  // Global illumination filter: if >80% of blocks changed, it's lighting
  // or camera tamper — track sustained global change for tamper detection
  if (changed_count > (VISION_GRID_TOTAL * 80 / 100)) {
    if (s_tamper_counter < 255) s_tamper_counter++;
    return result;
  }
  s_tamper_counter = 0;
  if (s_tamper_fired) {
    s_tamper_fired = false;
    s_stats.tamper_active = false;
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
static tflite::MicroMutableOpResolver<6>* s_resolver = nullptr;
static tflite::MicroInterpreter* s_interpreter = nullptr;
static bool s_model_loaded = false;

static void layer3_deinit() {
  delete s_interpreter; s_interpreter = nullptr;
  delete s_resolver;    s_resolver = nullptr;
  if (s_tensor_arena) { free(s_tensor_arena); s_tensor_arena = nullptr; }
  s_model_loaded = false;
}

static bool layer3_init() {
  layer3_deinit();

  s_tensor_arena = (uint8_t*)ps_malloc(kTensorArenaSize);
  if (!s_tensor_arena) {
    Serial.println("[VISION] Layer 3: PSRAM alloc failed for tensor arena");
    return false;
  }

  const tflite::Model* model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[VISION] Layer 3: model version mismatch");
    layer3_deinit();
    return false;
  }

  s_resolver = new tflite::MicroMutableOpResolver<6>();
  s_resolver->AddConv2D();
  s_resolver->AddDepthwiseConv2D();
  s_resolver->AddAveragePool2D();
  s_resolver->AddReshape();
  s_resolver->AddSoftmax();
  s_resolver->AddFullyConnected();
  s_interpreter = new tflite::MicroInterpreter(model, *s_resolver,
    s_tensor_arena, kTensorArenaSize);

  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[VISION] Layer 3: AllocateTensors failed");
    layer3_deinit();
    return false;
  }

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
  vision::s_longterm_set = false;
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
  if (vision::s_rgb_buf) {
    free(vision::s_rgb_buf);
    vision::s_rgb_buf = nullptr;
  }
#if FEATURE_VISION_TFLITE
  vision::layer3_deinit();
#endif
  vision::s_initialized = false;
}

bool vision_start() {
  if (!vision::s_initialized) return false;
  vision::s_running = true;
  vision::s_last_process_ms = millis();
  vision::s_duty_cycle_start = millis();
  vision::s_duty_resting = false;
  vision::s_consecutive_l2 = 0;
  Serial.println("[VISION] Detection started");
  return true;
}

void vision_stop() {
  if (vision::s_motion_active) {
    vision::s_motion_active = false;
    vision::s_stats.motion_active = false;
    vision::s_stats.last_confidence = 0;
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

static void decay_grid() {
  for (int i = 0; i < VISION_GRID_TOTAL; i++) {
    vision::s_stats.block_intensity[i] =
        (uint8_t)(vision::s_stats.block_intensity[i] * 3 / 4);
  }
}

bool vision_process() {
  if (!vision::s_running) return false;

  uint32_t now = millis();

  // Thermal duty cycle: alternate active/rest periods to limit sustained
  // CPU heat. duty_cycle_ms=10000 with duty_active_pct=50 means 5s active
  // then 5s rest.
  if (vision::s_duty_cycle_start == 0) vision::s_duty_cycle_start = now;
  uint32_t cycle_ms  = vision::s_cfg.duty_cycle_ms;
  uint32_t active_ms = cycle_ms * vision::s_cfg.duty_active_pct / 100;
  uint32_t elapsed   = now - vision::s_duty_cycle_start;
  if (elapsed >= cycle_ms) {
    vision::s_duty_cycle_start = now;
    vision::s_duty_resting = false;
    elapsed = 0;
  }
  if (elapsed >= active_ms && !vision::s_duty_resting) {
    vision::s_duty_resting = true;
  }
  if (vision::s_duty_resting) return false;

  // Rate limit: base interval, extended by backoff during sustained activity
  uint32_t interval = vision::s_cfg.process_interval_ms;
  if (vision::s_consecutive_l2 >= vision::s_cfg.sustained_threshold) {
    interval = vision::s_cfg.sustained_backoff_ms;
  }
  if (now - vision::s_last_process_ms < interval) return false;
  vision::s_last_process_ms = now;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) return false;

  if (cam.isPeekActive()) return false;

  if (cam.getThermalState() != THERMAL_NORMAL) return false;

  camera_fb_t* fb = cam.captureFrame();
  if (!fb) return false;

  vision::s_stats.frames_analyzed++;

  // ── Layer 1: JPEG size delta ──────────────────────────────────────
  bool l1_pass = vision::layer1_check(fb->len);
  if (!l1_pass && vision::s_tamper_counter == 0) {
    cam.returnFrame(fb);
    decay_grid();
    if (vision::s_motion_active &&
        now - vision::s_motion_start_ms > vision::s_cfg.motion_hold_ms) {
      vision::s_motion_active = false;
      vision::s_stats.motion_active = false;
      vision::s_stats.last_confidence = 0;
      vision::emit_event(VISION_EVENT_MOTION_END, 0, 0);
    }
    return false;
  }
  if (l1_pass) vision::s_stats.layer1_passes++;

  // ── Layer 2: Block luminance motion ───────────────────────────────
  bool decoded = vision::decode_and_downsample(fb, vision::s_gray_buf,
                                               DECODE_W, DECODE_H);
  cam.returnFrame(fb);

  if (!decoded) {
    decay_grid();
    return false;
  }

  vision::update_thumbnail();

  vision::MotionResult motion = vision::layer2_check(vision::s_gray_buf);

  // Scene tamper: sustained global illumination change (camera covered/moved)
  if (vision::s_tamper_counter >= vision::s_cfg.tamper_hold_frames &&
      !vision::s_tamper_fired) {
    vision::s_tamper_fired = true;
    vision::s_stats.tamper_active = true;
    vision::s_stats.tamper_events++;
    vision::emit_event(VISION_EVENT_TAMPER, 100, 0);
    log_health(LOG_LEVEL_WARN, LOG_CAT_SENSOR, "Vision: camera tamper detected", nullptr);
  }

  // Object removal: blocks reverted from diverged to stable long-term baseline
  if (motion.obj_removed) {
    vision::s_stats.obj_removed_events++;
    vision::emit_event(VISION_EVENT_OBJ_REMOVED, 80, motion.obj_removed_zone);
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR, "Vision: object removed from scene", nullptr);
  }

  if (!motion.detected) {
    vision::s_consecutive_l2 = 0;
    if (vision::s_motion_active &&
        now - vision::s_motion_start_ms > vision::s_cfg.motion_hold_ms) {
      vision::s_motion_active = false;
      vision::s_stats.motion_active = false;
      vision::s_stats.last_confidence = 0;
      vision::emit_event(VISION_EVENT_MOTION_END, 0, 0);
    }
    return motion.obj_removed;
  }
  vision::s_stats.layer2_passes++;
  if (vision::s_consecutive_l2 < 255) vision::s_consecutive_l2++;

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

bool vision_get_config(vision_config_t* out) {
  if (!out || !vision::s_initialized) return false;
  *out = vision::s_cfg;
  return true;
}

bool vision_set_config(const vision_config_t* cfg) {
  if (!cfg || !vision::s_initialized) return false;
  vision::s_cfg = *cfg;
  return true;
}

static const char* NVS_VISION_NS  = "scv_vis";
static const char* NVS_KEY_CONFIG = "cfg";

bool vision_save_config_to_nvs() {
  if (!vision::s_initialized) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_VISION_NS, false)) return false;
  size_t written = prefs.putBytes(NVS_KEY_CONFIG, &vision::s_cfg, sizeof(vision_config_t));
  prefs.end();
  Serial.printf("[VISION] Config saved to NVS (%u bytes)\n", (unsigned)written);
  return written == sizeof(vision_config_t);
}

bool vision_load_config_from_nvs(vision_config_t* out) {
  if (!out) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_VISION_NS, true)) return false;
  if (!prefs.isKey(NVS_KEY_CONFIG)) { prefs.end(); return false; }
  const size_t current_size = sizeof(vision_config_t);
  const size_t min_size = current_size - VISION_GRID_TOTAL;
  vision_config_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  size_t read = prefs.getBytes(NVS_KEY_CONFIG, &tmp, current_size);
  prefs.end();
  if (read == current_size) {
    *out = tmp;
    return true;
  }
  // Accept old (smaller) blobs: fields present are loaded, new trailing
  // fields (zone_sensitivity) remain zero-initialized from memset above.
  if (read >= min_size) {
    *out = tmp;
    Serial.printf("[VISION] NVS config migrated: %u -> %u bytes\n",
                  (unsigned)read, (unsigned)current_size);
    return true;
  }
  // Blob too old/corrupt — caller's struct is untouched (preserves defaults)
  return false;
}

bool vision_get_thumbnail(uint8_t* out, size_t cap) {
  if (!out || cap < VISION_THUMB_W * VISION_THUMB_H) return false;
  if (!vision::s_thumb_valid) return false;
  memcpy(out, vision::s_thumb, VISION_THUMB_W * VISION_THUMB_H);
  return true;
}

int vision_get_history(vision_history_entry_t* out, int max_entries) {
  if (!out || max_entries <= 0) return 0;
  portENTER_CRITICAL(&vision::s_history_mux);
  int count = vision::s_history_count;
  int head  = vision::s_history_head;
  int n = count < max_entries ? count : max_entries;
  int start = (head - count + VISION_HISTORY_SIZE) % VISION_HISTORY_SIZE;
  for (int i = 0; i < n; i++) {
    out[i] = vision::s_history[(start + (count - n) + i) % VISION_HISTORY_SIZE];
  }
  portEXIT_CRITICAL(&vision::s_history_mux);
  return n;
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
bool vision_get_config(vision_config_t*) { return false; }
bool vision_set_config(const vision_config_t*) { return false; }
bool vision_save_config_to_nvs() { return false; }
bool vision_load_config_from_nvs(vision_config_t*) { return false; }
bool vision_get_thumbnail(uint8_t*, size_t) { return false; }
int vision_get_history(vision_history_entry_t*, int) { return 0; }

#endif /* FEATURE_VISION_DETECT */
