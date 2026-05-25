/*
 * SecuraCV Canary — Camera Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_camera.h"
#include <Preferences.h>

#if FEATURE_CAMERA_PEEK

static const char* NVS_CAM_NS = "scv_cam";
static const char* NVS_KEY_HMIRROR = "hmirror";
static const char* NVS_KEY_VFLIP   = "vflip";

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static CameraManager s_camera;

CameraManager& camera_get_instance() {
  return s_camera;
}

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

const char* framesize_name(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA: return "160x120";
    case FRAMESIZE_QVGA:  return "320x240";
    case FRAMESIZE_CIF:   return "400x296";
    case FRAMESIZE_VGA:   return "640x480";
    case FRAMESIZE_SVGA:  return "800x600";
    case FRAMESIZE_XGA:   return "1024x768";
    case FRAMESIZE_HD:    return "1280x720";
    case FRAMESIZE_SXGA:  return "1280x1024";
    case FRAMESIZE_UXGA:  return "1600x1200";
    default: return "unknown";
  }
}

const char* sensor_model_name(uint16_t pid) {
  switch (pid) {
    case 0x2640: case 0x26:  return "OV2640";
    case 0x3660: case 0x36:  return "OV3660";
    case 0x5640:             return "OV5640";
    default:                 return "Unknown";
  }
}

// Apply one named field from a JSON object if present.
static bool apply_int_setting(sensor_t* s, const JsonObject& body, const char* key,
                              int (*setter)(sensor_t*, int), int min_val, int max_val) {
  if (!body[key].is<int>()) return false;
  int v = body[key].as<int>();
  if (v < min_val) v = min_val;
  if (v > max_val) v = max_val;
  setter(s, v);
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// CAMERA MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

CameraManager::CameraManager()
  : m_initialized(false), m_peek_active(false), m_framesize(FRAMESIZE_VGA),
    m_frame_delay_ms(40), m_metrics{}, m_metrics_mux(portMUX_INITIALIZER_UNLOCKED) {}

// Build a camera_config_t with all pin/clock fields populated. Critically
// zero-initialized so newer ESP-IDF fields (sccb_i2c_port, etc.) start clean
// instead of inheriting stack garbage — uninitialized sccb_i2c_port was the
// most common cause of "init returns ESP_OK but sensor probe fails" on
// XIAO ESP32S3 Sense boards built against Arduino-ESP32 v3 / IDF v5.
static camera_config_t make_base_config() {
  camera_config_t cfg = {};
  cfg.ledc_channel  = LEDC_CHANNEL_0;
  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0        = CAM_PIN_D0;
  cfg.pin_d1        = CAM_PIN_D1;
  cfg.pin_d2        = CAM_PIN_D2;
  cfg.pin_d3        = CAM_PIN_D3;
  cfg.pin_d4        = CAM_PIN_D4;
  cfg.pin_d5        = CAM_PIN_D5;
  cfg.pin_d6        = CAM_PIN_D6;
  cfg.pin_d7        = CAM_PIN_D7;
  cfg.pin_xclk      = CAM_PIN_XCLK;
  cfg.pin_pclk      = CAM_PIN_PCLK;
  cfg.pin_vsync     = CAM_PIN_VSYNC;
  cfg.pin_href      = CAM_PIN_HREF;
  cfg.pin_sccb_sda  = CAM_PIN_SIOD;
  cfg.pin_sccb_scl  = CAM_PIN_SIOC;
  cfg.pin_pwdn      = CAM_PIN_PWDN;
  cfg.pin_reset     = CAM_PIN_RESET;
  cfg.sccb_i2c_port = -1;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  return cfg;
}

void CameraManager::applyDefaultSensorTuning() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, 0);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, (gainceiling_t)GAINCEILING_4X);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_dcw(s, 1);
  s->set_hmirror(s, 0);
  s->set_vflip(s, 0);
  s->set_colorbar(s, 0);
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
}

bool CameraManager::begin() {
  psramInit();
  bool psram_ok = psramFound();
  Serial.printf("[CAMERA] PSRAM: %s\n", psram_ok ? "found" : "not found");

  camera_config_t attempts[3];
  const char*     labels[3];
  int n = 0;

  if (psram_ok) {
    camera_config_t a = make_base_config();
    a.frame_size  = FRAMESIZE_XGA;
    a.jpeg_quality = 10;
    a.fb_count    = 2;
    a.fb_location = CAMERA_FB_IN_PSRAM;
    a.grab_mode   = CAMERA_GRAB_LATEST;
    attempts[n] = a; labels[n] = "psram-xga"; n++;

    camera_config_t b = make_base_config();
    b.frame_size  = FRAMESIZE_VGA;
    b.jpeg_quality = 12;
    b.fb_count    = 1;
    b.fb_location = CAMERA_FB_IN_PSRAM;
    b.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    attempts[n] = b; labels[n] = "psram-vga"; n++;
  }

  camera_config_t c = make_base_config();
  c.frame_size  = FRAMESIZE_QVGA;
  c.jpeg_quality = 12;
  c.fb_count    = 1;
  c.fb_location = CAMERA_FB_IN_DRAM;
  c.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
  attempts[n] = c; labels[n] = "dram-qvga"; n++;

  esp_err_t err = ESP_FAIL;
  int chosen = -1;
  for (int i = 0; i < n; i++) {
    err = esp_camera_init(&attempts[i]);
    if (err == ESP_OK) { chosen = i; break; }
    Serial.printf("[CAMERA] Init failed (%s): 0x%x — retrying\n", labels[i], err);
    esp_camera_deinit();
    delay(100);
  }

  if (chosen < 0) {
    Serial.printf("[CAMERA] All init attempts failed (last err=0x%x)\n", err);
    m_initialized = false;
    return false;
  }

  m_framesize = attempts[chosen].frame_size;
  m_initialized = true;
  applyDefaultSensorTuning();
  loadOrientationFromNvs();
  Serial.printf("[CAMERA] Initialized (%s) for peek/preview\n", labels[chosen]);
  return true;
}

void CameraManager::end() {
  if (m_initialized) {
    esp_camera_deinit();
    m_initialized = false;
    m_peek_active = false;
  }
}

bool CameraManager::reinit() {
  if (m_peek_active) {
    m_peek_active = false;
    delay(150);
  }
  end();
  return begin();
}

bool CameraManager::setResolution(framesize_t size) {
  if (!m_initialized) return false;

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;

  if (s->set_framesize(s, size) != 0) {
    return false;
  }

  m_framesize = size;
  return true;
}

const char* CameraManager::getResolutionName() const {
  return framesize_name(m_framesize);
}

camera_fb_t* CameraManager::captureFrame() {
  if (!m_initialized) return nullptr;
  return esp_camera_fb_get();
}

void CameraManager::returnFrame(camera_fb_t* fb) {
  if (fb) {
    esp_camera_fb_return(fb);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// STREAM METRICS
// ════════════════════════════════════════════════════════════════════════════

void CameraManager::resetMetrics() {
  uint32_t now = millis();
  portENTER_CRITICAL(&m_metrics_mux);
  memset(&m_metrics, 0, sizeof(m_metrics));
  m_metrics.stream_start_ms  = now;
  m_metrics.fps_window_start = now;
  portEXIT_CRITICAL(&m_metrics_mux);
}

void CameraManager::recordFrame(uint32_t frame_bytes) {
  uint32_t t_ms = millis();
  portENTER_CRITICAL(&m_metrics_mux);
  m_metrics.last_frame_bytes = frame_bytes;
  m_metrics.last_frame_ms    = t_ms;
  m_metrics.total_bytes     += frame_bytes;
  m_metrics.frame_count++;
  m_metrics.fps_window_count++;
  uint32_t window_elapsed = t_ms - m_metrics.fps_window_start;
  if (window_elapsed >= 1000) {
    m_metrics.fps_last         = (m_metrics.fps_window_count * 1000U) / window_elapsed;
    m_metrics.fps_window_count = 0;
    m_metrics.fps_window_start = t_ms;
  }
  portEXIT_CRITICAL(&m_metrics_mux);
}

PeekMetrics CameraManager::snapshotMetrics() {
  PeekMetrics copy;
  portENTER_CRITICAL(&m_metrics_mux);
  copy = m_metrics;
  portEXIT_CRITICAL(&m_metrics_mux);
  return copy;
}

void CameraManager::setFrameDelay(uint32_t ms) {
  if (ms < 20)  ms = 20;
  if (ms > 500) ms = 500;
  m_frame_delay_ms = ms;
}

// ════════════════════════════════════════════════════════════════════════════
// SENSOR INFO
// ════════════════════════════════════════════════════════════════════════════

uint16_t CameraManager::getSensorPID() const {
  sensor_t* s = esp_camera_sensor_get();
  return s ? s->id.PID : 0;
}

const char* CameraManager::getSensorModelName() const {
  return sensor_model_name(getSensorPID());
}

// ════════════════════════════════════════════════════════════════════════════
// SENSOR PARAMETER READ / WRITE
// ════════════════════════════════════════════════════════════════════════════

bool CameraManager::getSensorParams(JsonDocument& doc) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;

  doc["ok"]              = true;
  doc["sensor_pid"]      = s->id.PID;
  doc["sensor_model"]    = sensor_model_name(s->id.PID);
  doc["framesize"]       = s->status.framesize;
  doc["quality"]         = s->status.quality;
  doc["brightness"]      = s->status.brightness;
  doc["contrast"]        = s->status.contrast;
  doc["saturation"]      = s->status.saturation;
  doc["sharpness"]       = s->status.sharpness;
  doc["denoise"]         = s->status.denoise;
  doc["special_effect"]  = s->status.special_effect;
  doc["wb_mode"]         = s->status.wb_mode;
  doc["awb"]             = s->status.awb;
  doc["awb_gain"]        = s->status.awb_gain;
  doc["aec"]             = s->status.aec;
  doc["aec2"]            = s->status.aec2;
  doc["ae_level"]        = s->status.ae_level;
  doc["aec_value"]       = s->status.aec_value;
  doc["agc"]             = s->status.agc;
  doc["agc_gain"]        = s->status.agc_gain;
  doc["gainceiling"]     = s->status.gainceiling;
  doc["bpc"]             = s->status.bpc;
  doc["wpc"]             = s->status.wpc;
  doc["raw_gma"]         = s->status.raw_gma;
  doc["lenc"]            = s->status.lenc;
  doc["hmirror"]         = s->status.hmirror;
  doc["vflip"]           = s->status.vflip;
  doc["dcw"]             = s->status.dcw;
  doc["colorbar"]        = s->status.colorbar;
  doc["frame_delay_ms"]  = m_frame_delay_ms;

  return true;
}

bool CameraManager::applySensorParams(const JsonObject& obj) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;

  // JPEG quality — also read back from sensor
  if (obj["quality"].is<int>()) {
    int q = obj["quality"].as<int>();
    if (q < 0) q = 0; if (q > 63) q = 63;
    s->set_quality(s, q);
  }

  // Image tuning (range -2..2)
  apply_int_setting(s, obj, "brightness",     s->set_brightness,     -2,  2);
  apply_int_setting(s, obj, "contrast",       s->set_contrast,       -2,  2);
  apply_int_setting(s, obj, "saturation",     s->set_saturation,     -2,  2);
  apply_int_setting(s, obj, "sharpness",      s->set_sharpness,      -2,  2);
  apply_int_setting(s, obj, "denoise",        s->set_denoise,         0, 255);
  apply_int_setting(s, obj, "special_effect", s->set_special_effect,  0,  6);
  apply_int_setting(s, obj, "ae_level",       s->set_ae_level,       -2,  2);

  // White balance
  apply_int_setting(s, obj, "wb_mode",        s->set_wb_mode,         0,  4);
  apply_int_setting(s, obj, "awb",            s->set_whitebal,        0,  1);
  apply_int_setting(s, obj, "awb_gain",       s->set_awb_gain,        0,  1);

  // Exposure
  apply_int_setting(s, obj, "aec",            s->set_exposure_ctrl,   0,  1);
  apply_int_setting(s, obj, "aec2",           s->set_aec2,            0,  1);
  apply_int_setting(s, obj, "aec_value",      s->set_aec_value,       0, 1200);

  // Gain
  apply_int_setting(s, obj, "agc",            s->set_gain_ctrl,       0,  1);
  apply_int_setting(s, obj, "agc_gain",       s->set_agc_gain,        0, 30);
  if (obj["gainceiling"].is<int>()) {
    int g = obj["gainceiling"].as<int>();
    if (g < 0) g = 0; if (g > 6) g = 6;
    s->set_gainceiling(s, (gainceiling_t)g);
  }

  // Image cleanup
  apply_int_setting(s, obj, "bpc",            s->set_bpc,             0,  1);
  apply_int_setting(s, obj, "wpc",            s->set_wpc,             0,  1);
  apply_int_setting(s, obj, "raw_gma",        s->set_raw_gma,         0,  1);
  apply_int_setting(s, obj, "lenc",           s->set_lenc,            0,  1);
  apply_int_setting(s, obj, "dcw",            s->set_dcw,             0,  1);

  // Orientation — persisted to NVS (physical mounting constants)
  bool orientation_changed = false;
  if (apply_int_setting(s, obj, "hmirror", s->set_hmirror, 0, 1)) orientation_changed = true;
  if (apply_int_setting(s, obj, "vflip",   s->set_vflip,   0, 1)) orientation_changed = true;
  if (orientation_changed) saveOrientationToNvs();

  apply_int_setting(s, obj, "colorbar",       s->set_colorbar,        0,  1);

  // Stream pacing (not a sensor setting, but lives here so the UI can tune FPS)
  if (obj["frame_delay_ms"].is<int>()) {
    setFrameDelay(obj["frame_delay_ms"].as<int>());
  }

  // Reset to surveillance defaults if requested (preserves saved orientation)
  if (obj["reset_defaults"].is<bool>() && obj["reset_defaults"].as<bool>()) {
    applyDefaultSensorTuning();
    loadOrientationFromNvs();
    m_frame_delay_ms = 40;
  }

  return true;
}

void CameraManager::resetSensorDefaults() {
  applyDefaultSensorTuning();
  loadOrientationFromNvs();
  m_frame_delay_ms = 40;
}

// ════════════════════════════════════════════════════════════════════════════
// ORIENTATION NVS PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

void CameraManager::loadOrientationFromNvs() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  Preferences prefs;
  if (!prefs.begin(NVS_CAM_NS, true)) return;

  if (prefs.isKey(NVS_KEY_HMIRROR)) {
    s->set_hmirror(s, prefs.getUChar(NVS_KEY_HMIRROR, 0));
  }
  if (prefs.isKey(NVS_KEY_VFLIP)) {
    s->set_vflip(s, prefs.getUChar(NVS_KEY_VFLIP, 0));
  }
  prefs.end();
}

void CameraManager::saveOrientationToNvs() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  Preferences prefs;
  if (!prefs.begin(NVS_CAM_NS, false)) return;

  prefs.putUChar(NVS_KEY_HMIRROR, s->status.hmirror ? 1 : 0);
  prefs.putUChar(NVS_KEY_VFLIP,   s->status.vflip   ? 1 : 0);
  prefs.end();
  Serial.printf("[CAMERA] Orientation saved: hmirror=%d vflip=%d\n",
                s->status.hmirror, s->status.vflip);
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool camera_init() {
  return camera_get_instance().begin();
}

bool camera_is_initialized() {
  return camera_get_instance().isInitialized();
}

bool camera_is_peek_active() {
  return camera_get_instance().isPeekActive();
}

void camera_set_peek_active(bool active) {
  camera_get_instance().setPeekActive(active);
}

bool camera_reinit() {
  return camera_get_instance().reinit();
}

#endif // FEATURE_CAMERA_PEEK
