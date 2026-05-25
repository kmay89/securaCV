/*
 * SecuraCV Canary — Camera Management
 *
 * Camera initialization, MJPEG streaming, sensor tuning, and peek/preview.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CAMERA_H
#define SECURACV_CAMERA_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "canary_config.h"

#if FEATURE_CAMERA_PEEK

#include "esp_camera.h"

// ════════════════════════════════════════════════════════════════════════════
// STREAM METRICS
// ════════════════════════════════════════════════════════════════════════════

struct PeekMetrics {
  uint32_t frame_count;
  uint32_t last_frame_bytes;
  uint32_t last_frame_ms;
  uint32_t stream_start_ms;
  uint64_t total_bytes;
  uint32_t fps_window_start;
  uint32_t fps_window_count;
  uint32_t fps_last;
};

// ════════════════════════════════════════════════════════════════════════════
// CAMERA MANAGER
// ════════════════════════════════════════════════════════════════════════════

class CameraManager {
public:
  CameraManager();

  // Initialize / deinitialize camera
  bool begin();
  void end();
  bool reinit();

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

  // Stream metrics (spinlock-protected for thread safety)
  void resetMetrics();
  void recordFrame(uint32_t frame_bytes);
  PeekMetrics snapshotMetrics();

  // Frame pacing
  uint32_t getFrameDelay() const { return m_frame_delay_ms; }
  void setFrameDelay(uint32_t ms);

  // Sensor info
  uint16_t getSensorPID() const;
  const char* getSensorModelName() const;

  // Sensor parameter read/write — single source of truth for validation
  bool getSensorParams(JsonDocument& doc);
  bool applySensorParams(const JsonObject& obj);
  void resetSensorDefaults();

private:
  void applyDefaultSensorTuning();
  void loadOrientationFromNvs();
  void saveOrientationToNvs();

  bool m_initialized;
  volatile bool m_peek_active;
  framesize_t m_framesize;
  uint32_t m_frame_delay_ms;

  PeekMetrics m_metrics;
  portMUX_TYPE m_metrics_mux;
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
bool camera_reinit();

// Resolution name lookup
const char* framesize_name(framesize_t size);

// Sensor model name from PID
const char* sensor_model_name(uint16_t pid);

#endif // FEATURE_CAMERA_PEEK

#endif // SECURACV_CAMERA_H
