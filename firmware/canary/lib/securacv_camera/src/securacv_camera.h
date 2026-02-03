/*
 * SecuraCV Canary — Camera Management
 *
 * Camera initialization, MJPEG streaming, and peek/preview.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CAMERA_H
#define SECURACV_CAMERA_H

#include <Arduino.h>
#include "canary_config.h"

#if FEATURE_CAMERA_PEEK

#include "esp_camera.h"

// ════════════════════════════════════════════════════════════════════════════
// CAMERA MANAGER
// ════════════════════════════════════════════════════════════════════════════

class CameraManager {
public:
  CameraManager();

  // Initialize camera
  bool begin();

  // Deinitialize camera
  void end();

  // Status
  bool isInitialized() const { return m_initialized; }
  bool isPeekActive() const { return m_peek_active; }

  // Peek control
  void setPeekActive(bool active) { m_peek_active = active; }

  // Resolution control
  bool setResolution(framesize_t size);
  framesize_t getResolution() const { return m_framesize; }
  const char* getResolutionName() const;

  // Capture single frame
  camera_fb_t* captureFrame();
  void returnFrame(camera_fb_t* fb);

private:
  bool m_initialized;
  volatile bool m_peek_active;
  framesize_t m_framesize;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

CameraManager& camera_get_instance();

// Convenience functions
bool camera_init();
bool camera_is_initialized();
bool camera_is_peek_active();
void camera_set_peek_active(bool active);

// Resolution name lookup
const char* framesize_name(framesize_t size);

#endif // FEATURE_CAMERA_PEEK

#endif // SECURACV_CAMERA_H
