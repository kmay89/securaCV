/**
 * @file camera_gate_logic.h
 * @brief Pure decisions for camera power management: when the peek
 *        surface may run, and when the idle sensor should enter standby.
 *
 * The OV2640 on the XIAO ESP32-S3 Sense has no wired power-down pin, so
 * "standby" means esp_camera_deinit() (stops the 20 MHz XCLK, frees the
 * framebuffers) and "wake" re-runs the boot init ladder (~1 s). The
 * runtime glue (mutex, init/deinit, health logs) lives in canary_wap.ino;
 * every branchy DECISION lives here so a host g++ run
 * (test_camera_gate_logic.cpp) pins the contract:
 *
 *  - PEEK (stream/snapshot) is a convenience surface: it yields to the
 *    battery policy and to thermal protection, and is refused with an
 *    honest reason instead of failing silently.
 *  - VAULT captures and QR provisioning are NOT gated here: life-safety
 *    evidence and explicit user actions always may wake the camera.
 *
 * Hosted C++ only — no Arduino includes. Names carry a CAM_/camera_
 * prefix; short ALL-CAPS names (LINE_MAX!) collide with newlib macros
 * on-target.
 */

#ifndef CAMERA_GATE_LOGIC_H
#define CAMERA_GATE_LOGIC_H

#include <stdint.h>

namespace camera_gate {

/* Idle window before an unused camera is put in standby. Wake costs ~1 s,
 * so this is deliberately generous — the win is idle current and heat on
 * a device that may go days between camera uses. */
constexpr uint32_t CAM_IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;

enum class PeekGate : uint8_t {
  ALLOW          = 0,
  DENY_POLICY    = 1,  /* battery power policy has camera_peek off */
  DENY_THERMAL   = 2,  /* die critically hot — shedding the heat source */
  DENY_NO_CAMERA = 3,  /* init failed and the sensor is not in standby */
};

/* Short machine name for JSON fields and UI logic. */
inline const char* peek_gate_name(PeekGate g) {
  switch (g) {
    case PeekGate::ALLOW:        return "ok";
    case PeekGate::DENY_POLICY:  return "policy";
    case PeekGate::DENY_THERMAL: return "thermal";
    default:                     return "no_camera";
  }
}

inline const char* peek_gate_reason(PeekGate g) {
  switch (g) {
    case PeekGate::ALLOW:
      return "ok";
    case PeekGate::DENY_POLICY:
      return "Camera preview is off to save battery. Plug in to use it.";
    case PeekGate::DENY_THERMAL:
      return "Device is too hot. The preview stays off until it cools.";
    default:
      return "Camera did not start. Check the sensor and try Reinit.";
  }
}

/* Order matters and is part of the contract: a broken camera reports as
 * broken even when the policy would also deny (the user should fix the
 * real problem first); thermal outranks policy (it is the acute one). */
inline PeekGate peek_gate(bool camera_usable, bool policy_allows,
                          bool hot_critical) {
  if (!camera_usable) return PeekGate::DENY_NO_CAMERA;
  if (hot_critical)   return PeekGate::DENY_THERMAL;
  if (!policy_allows) return PeekGate::DENY_POLICY;
  return PeekGate::ALLOW;
}

/* Should the initialized-but-unused sensor be put in standby now?
 * - never while any camera consumer is active (stream / QR / seal);
 * - immediately when the battery policy has the peek surface off (the
 *   sensor will be woken on demand by vault/QR anyway);
 * - otherwise after CAM_IDLE_TIMEOUT_MS without use (wrap-safe millis
 *   subtraction — millis wraps every ~49.7 days). */
inline bool standby_due(uint32_t now_ms, uint32_t last_use_ms,
                        bool initialized, bool in_use, bool policy_allows,
                        uint32_t idle_timeout_ms = CAM_IDLE_TIMEOUT_MS) {
  if (!initialized) return false;
  if (in_use) return false;
  if (!policy_allows) return true;
  return (uint32_t)(now_ms - last_use_ms) >= idle_timeout_ms;
}

}  // namespace camera_gate

#endif  // CAMERA_GATE_LOGIC_H
