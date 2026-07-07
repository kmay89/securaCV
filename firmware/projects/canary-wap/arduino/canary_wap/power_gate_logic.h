/**
 * @file power_gate_logic.h
 * @brief Pure decisions for "battery gates round two" — which advisory
 *        power-policy feature bits the main loop actually enforces, and
 *        how routine MQTT heartbeat cadence stretches under battery load.
 *
 * PR #847 wired the first enforced gates (camera_peek, record-interval
 * floor, CPU/WiFi-PS, deep sleep). This header holds the round-two
 * decisions so they are host-testable with no Arduino/ESP dependency, the
 * same split camera_gate_logic.h uses. The sketch reads
 * power_policy::get_features()/get_mode() and feeds the primitives here.
 *
 * Scope (deliberate, see the battery guide + power_policy.h enforcement
 * comment, kept in sync):
 *   - CSI drain: enforced. CSI is pure environmental sensing (no
 *     life-safety), so honoring the `csi` bit stops the pipeline under
 *     battery load exactly as the profiles intend.
 *   - MQTT routine heartbeats (status / health / mesh-snapshot / beacon):
 *     cadence stretched by power mode. The `mqtt` bit is TRUE in every
 *     mode (LOW_POWER keeps STA+MQTT alive so panic events reach HA), so
 *     the honest battery lever is lengthening ROUTINE heartbeats — never
 *     skipping them, never touching the link. Life-safety (acoustic
 *     /sensing) and event-driven (counts/chain) publishes bypass this.
 *   - Mesh servicing (mesh_network::update / chirp): intentionally NOT
 *     gated off — mesh carries inter-canary security/tamper alerts, and
 *     dropping alert reception to save a little CPU is the wrong trade
 *     for a security device (cf. acoustic, kept on in every mode). Only
 *     the routine mesh *publish* cadence is stretched, via the same
 *     heartbeat helper. `mesh` stays advisory in the guide with that
 *     rationale.
 *
 * Named without ALL-CAPS host-clashing identifiers (POSIX macro hygiene,
 * same caution as camera_gate_logic.h).
 */

#ifndef POWER_GATE_LOGIC_H
#define POWER_GATE_LOGIC_H

#include <stdint.h>

namespace power_gate {

// Mirror of PowerPolicyMode (power_policy.h). The sketch static_asserts
// these against the real enum at the call site so the mapping cannot
// silently drift; the pure header stays free of the Arduino-side header.
static constexpr uint8_t MODE_PLUGGED_IN     = 0;
static constexpr uint8_t MODE_BATTERY_NORMAL = 1;
static constexpr uint8_t MODE_BATTERY_SAVER  = 2;
static constexpr uint8_t MODE_LOW_POWER      = 3;
static constexpr uint8_t MODE_SHUTDOWN       = 4;
static constexpr uint8_t MODE_USB_ONLY       = 5;

/**
 * Should a policy-advisory subsystem run this loop? When no policy engine
 * is compiled in the sketch passes has_policy=false and the feature runs
 * (the compile-time FEATURE_* flag still gates it). With a policy present,
 * the runtime bit decides. Mirrors camera_policy_allows_peek()'s shape.
 */
inline bool feature_runs(bool has_policy, bool feature_bit) {
  return !has_policy || feature_bit;
}

/**
 * Per-mode multiplier for ROUTINE MQTT heartbeat cadence. 1x on
 * mains/normal battery (stay responsive); progressively longer as the
 * battery drains. LOW_POWER/SHUTDOWN keep the link but heartbeat rarely.
 * Unknown modes fall back to 1x (fail-responsive, never faster).
 */
inline uint32_t routine_stretch_factor(uint8_t mode) {
  switch (mode) {
    case MODE_BATTERY_SAVER: return 4;
    case MODE_LOW_POWER:     return 8;
    case MODE_SHUTDOWN:      return 8;
    default:                 return 1;  // PLUGGED_IN, BATTERY_NORMAL, USB_ONLY
  }
}

/**
 * Effective interval for a routine heartbeat: base_ms stretched by the
 * mode factor, saturating at UINT32_MAX rather than wrapping on an absurd
 * base. Life-safety and event-driven publishes must NOT be routed through
 * this — they publish on their own triggers.
 */
inline uint32_t routine_interval_ms(uint32_t base_ms, uint8_t mode) {
  const uint32_t factor = routine_stretch_factor(mode);
  if (factor <= 1) return base_ms;
  if (base_ms > 0xFFFFFFFFu / factor) return 0xFFFFFFFFu;
  return base_ms * factor;
}

}  // namespace power_gate

#endif  // POWER_GATE_LOGIC_H
