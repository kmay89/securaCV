/*
 * SecuraCV Canary — Camera Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_camera.h"
#include "securacv_witness.h"

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

bool CameraManager::begin() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // Adjust for PSRAM availability
  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAMERA] Init failed: 0x%x\n", err);
    m_initialized = false;
    return false;
  }

  m_framesize = config.frame_size;
  m_initialized = true;
  Serial.println("[CAMERA] Initialized for peek/preview");
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
