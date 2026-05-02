/*
 * SecuraCV Canary — Camera Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_camera.h"

#if FEATURE_CAMERA_PEEK

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

// ════════════════════════════════════════════════════════════════════════════
// CAMERA MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

CameraManager::CameraManager()
  : m_initialized(false), m_peek_active(false), m_framesize(FRAMESIZE_VGA) {}

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

// Apply the same surveillance-tuned defaults as the WAP firmware so the new
// lib-based image matches behavior once it ships.
static void apply_default_sensor_tuning() {
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
  // Sensor-specific corrections last so they win over the neutral defaults.
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
}

bool CameraManager::begin() {
  // Force PSRAM init in case the board variant left it disabled. No-op on
  // boards where the ROM already turned PSRAM on.
  psramInit();
  bool psram_ok = psramFound();
  Serial.printf("[CAMERA] PSRAM: %s\n", psram_ok ? "found" : "not found");

  // Multi-stage attempt: most-capable → most-conservative.
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
  apply_default_sensor_tuning();
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

#endif // FEATURE_CAMERA_PEEK
