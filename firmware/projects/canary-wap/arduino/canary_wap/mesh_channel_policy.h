/*
 * SecuraCV Canary — Mesh Channel Policy
 *
 * Decides which 2.4 GHz channel ESP-NOW (Opera) and the chirp broadcast share.
 *
 * Why this module exists
 * ──────────────────────
 * The ESP32 has ONE 2.4 GHz radio. ESP-NOW therefore operates on whatever
 * channel the WiFi MAC is currently tuned to. Earlier code hard-pinned ESP-NOW
 * peer entries to channel 1 (Opera) and channel 6 (chirp). When a Canary is
 * also in STA mode on the user's home WiFi (a different channel), each ESP-NOW
 * send would fight the STA channel — degrading the home network *and* the
 * mesh. That is the "interferes with regular WiFi" failure mode the user
 * called out.
 *
 * Policy
 * ──────
 *   - STA connected  → mesh follows the STA channel (radio is already there).
 *   - STA off, AP on → mesh follows the AP channel.
 *   - Both off       → mesh picks MESH_FALLBACK_CHANNEL (default 6).
 *
 * All ESP-NOW peer entries should be registered with `peer.channel = 0`, which
 * the ESP-IDF treats as "use current radio channel." This module exists to:
 *   1. Provide a single source of truth for the *effective* mesh channel
 *      (for telemetry, BLE console, the /admin UI, and HA sensors).
 *   2. Drive `esp_wifi_set_channel()` when the radio is idle (no STA / no AP)
 *      so the mesh doesn't accidentally sit on a stale channel.
 *   3. Notify subscribers when the effective channel changes, so dynamic
 *      backends (chirp, opera) can update any cached state.
 *
 * This header is intentionally Arduino-free in its core logic so it can be
 * unit-tested on a host build. ESP32-specific glue is gated on
 * SECURACV_MCP_HAS_ESP32 (set by the firmware build).
 */

#ifndef SECURACV_MESH_CHANNEL_POLICY_H
#define SECURACV_MESH_CHANNEL_POLICY_H

#include <stdint.h>
#include <stddef.h>

namespace mesh_channel_policy {

// 2.4 GHz channel used when neither STA nor AP is up. 6 sits mid-band and
// is the most common ISM/WiFi non-overlapping middle pick.
static const uint8_t MESH_FALLBACK_CHANNEL = 6;

// Inputs to the policy decision. The firmware fills these in from
// WiFi.status() / hal_wifi_get_status() each time the radio state changes.
struct RadioState {
  bool sta_connected;     // STA has a working association
  uint8_t sta_channel;    // 1-13 (2.4 GHz) or 36-165 (5 GHz)
  bool ap_active;         // softAP up
  uint8_t ap_channel;     // 1-13
};

// Output of the policy decision.
struct ChannelDecision {
  uint8_t channel;        // The channel mesh + chirp should operate on
  bool locked_to_sta;     // True if forced to STA channel
  bool locked_to_ap;      // True if forced to AP channel (STA off)
  bool fallback;          // True if neither STA nor AP is up
};

// Pure decision function — no I/O, host-testable.
ChannelDecision decide(const RadioState& state);

// Convenience accessor used by other modules. On firmware builds this samples
// the current radio state; on host builds it returns the last value set with
// set_state_for_tests().
ChannelDecision current();

// Change-notification subscription. The firmware calls poll_radio() once per
// loop iteration; if the decided channel has changed since the previous poll,
// every registered listener is invoked exactly once. Listeners must be cheap
// (they run in the main loop context, no blocking I/O).
typedef void (*ChannelChangedCallback)(uint8_t old_channel, uint8_t new_channel);
bool register_listener(ChannelChangedCallback cb);
void poll_radio();

// Host-test seam. The firmware build ignores this; the host build uses it to
// drive RadioState into the module without an ESP32 around.
void set_state_for_tests(const RadioState& state);

} // namespace mesh_channel_policy

#endif // SECURACV_MESH_CHANNEL_POLICY_H
