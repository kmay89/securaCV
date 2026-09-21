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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
// THERMAL STATE
// ════════════════════════════════════════════════════════════════════════════

enum ThermalState : uint8_t {
  THERMAL_NORMAL    = 0,
  THERMAL_THROTTLED = 1,
  THERMAL_PAUSED    = 2
};

/* THERMAL_THROTTLE_TEMP_C / THERMAL_PAUSE_TEMP_C / THERMAL_RECOVER_MARGIN_C
 * come from canary_config.h (single source, shared with the thermal
 * watchdog's shadow classifier). */
#define THERMAL_CHECK_INTERVAL_MS  5000
#define THERMAL_FAIL_SAFE_COUNT    3     // consecutive read failures before fail-safe throttle
#define FREEZE_TIMEOUT_MS          10000

// ════════════════════════════════════════════════════════════════════════════
// CAMERA MANAGER
// ════════════════════════════════════════════════════════════════════════════

/* Three tasks touch the esp_camera_* driver: the loop task (vision capture,
 * power-policy deinit), the dedicated peek-stream task (capture + freeze
 * recovery), and the httpd workers (re-init endpoint, snapshot capture).
 * The flag guards (`isPeekActive`, `isInitialized`) order the common cases
 * but cannot close the window between a check and the driver call, so every
 * lifecycle transition and every capture goes through one lifecycle mutex:
 *
 *   - captureFrame() takes the lock (short timeout → nullptr on miss) and
 *     HOLDS it until returnFrame() gives it back — the frame buffer points
 *     into driver memory, so deinit must be excluded for the frame's whole
 *     lifetime, including the socket send. Capture and return are always
 *     called from the same task (FreeRTOS mutexes require owner-give).
 *   - begin()/end()/reinit()/setResolution() and checkFreeze()'s recovery
 *     take the lock with a longer timeout and SKIP (fail soft, caller
 *     retries) when it's busy — never block a task toward the 8 s task
 *     watchdog.
 */
class CameraManager {
public:
  CameraManager();

  // Initialize / deinitialize camera. Serialized by the lifecycle lock;
  // fail soft (begin() false, end() a no-op) if the lock is busy — check
  // isInitialized() and retry.
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

  // Capture single frame. A non-null frame comes back with the lifecycle
  // lock HELD — the same task must call returnFrame() to release both the
  // buffer and the lock. nullptr means not initialized, lock busy, or
  // driver failure; no lock is held in that case.
  camera_fb_t* captureFrame();
  void returnFrame(camera_fb_t* fb);

  // Stream metrics (spinlock-protected for thread safety)
  void resetMetrics();
  void recordFrame(uint32_t frame_bytes);
  PeekMetrics snapshotMetrics();

  // Frame pacing (returns thermal-adjusted value during throttling)
  uint32_t getFrameDelay() const;
  void setFrameDelay(uint32_t ms);
  uint32_t getUserFrameDelay() const { return m_user_frame_delay_ms; }

  // Thermal management — call periodically during streaming
  void checkThermal();
  ThermalState getThermalState() const { return m_thermal_state; }
  int8_t getDieTempC() const { return m_die_temp_c; }
  bool getThermalSensorOk() const { return m_thermal_fail_count < THERMAL_FAIL_SAFE_COUNT; }

  // Freeze detection — call when frame capture fails
  bool checkFreeze(uint32_t now_ms);
  uint16_t getFreezeCount() const { return m_freeze_count; }

  // Sensor info. Serialized by the lifecycle lock — esp_camera_sensor_get()
  // hands back driver-owned state that deinit frees, so every dereference
  // must exclude teardown. A busy lock reads as PID 0 / "Unknown".
  uint16_t getSensorPID() const;
  const char* getSensorModelName() const;

  // Sensor parameter read/write — single source of truth for validation.
  // Same lifecycle-lock rule as above; false on a busy lock or torn-down
  // camera (callers already treat false as "camera unavailable").
  bool getSensorParams(JsonDocument& doc);
  bool applySensorParams(const JsonObject& obj);
  void resetSensorDefaults();
  bool applyPreset(const char* name);

private:
  void applyDefaultSensorTuning();
  void loadOrientationFromNvs();
  void saveOrientationToNvs();

  // Lifecycle lock plumbing. beginLocked()/endLocked() are the raw
  // transitions for callers that already own the lock (reinit, freeze
  // recovery) — public begin()/end() are take-lock wrappers around them.
  // const: taking/giving the semaphore mutates the semaphore, not this
  // object, so const readers (getSensorPID) can serialize too.
  bool lockTake(uint32_t timeout_ms) const;
  void lockGive() const;
  bool beginLocked();
  void endLocked();

  SemaphoreHandle_t m_lock;
  bool m_initialized;
  volatile bool m_peek_active;
  framesize_t m_framesize;
  uint32_t m_frame_delay_ms;
  uint32_t m_user_frame_delay_ms;

  PeekMetrics m_metrics;
  portMUX_TYPE m_metrics_mux;

  // Thermal state
  ThermalState m_thermal_state;
  int8_t m_die_temp_c;
  uint32_t m_last_thermal_check_ms;
  uint8_t m_thermal_fail_count;

  // Freeze detection
  uint32_t m_last_good_frame_ms;
  uint16_t m_freeze_count;
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
