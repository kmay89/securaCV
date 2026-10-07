/**
 * @file improv_ble.h
 * @brief Improv Wi-Fi over BLE — the NimBLE glue around improv_core.h.
 *
 * One GATT service, registered on the board's NimBLE server (created here
 * when the board has none — the beacon-only Sense and Vision), that lets a
 * phone hand a Canary its Wi-Fi with one tap. The rules and the bytes are in
 * improv_core.h and host-tested there; this file only moves them on and off
 * the radio and hands the credentials to the board's own join path.
 *
 * The door is the shared setup portal's: this module is told each loop pass
 * whether the portal is up (tick's `no_wifi`), and that is exactly when the
 * Improv door is open. The join itself goes through
 * setup_portal_submit_join(), so the SoftAP wizard and the Bluetooth door
 * share one Testing pass, one persist-on-success rule and one teardown.
 *
 * Advertising: the fleet presence beacon stays the PRIMARY advert at all
 * times (a display's passive roster scan keeps hearing it; the bytes a
 * provisioned device puts on air are unchanged). While the door is open the
 * beacon carries FLEET_BEACON_FLAG_SETUP_OPEN and a short local name, the
 * device is connectable, and the scan response carries the Improv service
 * UUID plus its 0x4677 service data — which is where iOS, Android and
 * Chrome look for it (each merges the scan response into the advertisement
 * a service filter matches). Door shut: beacon only, no scan response, and
 * on a board with no other service, not connectable — nothing to dial.
 * The beacon carrier calls advertise() with its bytes instead of touching
 * NimBLE itself.
 *
 * Cross-core: NimBLE-Arduino 2.x (core 3.x: Sense) and 1.4.x (core 2.x:
 * Vision) differ in callback and advertising signatures; the
 * ESP_ARDUINO_VERSION_MAJOR split mirrors fleet_beacon_adv.cpp.
 *
 * Compiled by the family envs through their build_src_filter beside
 * setup_portal.cpp (scripts/lint_common_lib_manifests.py explains why a
 * path-prefixed include must be named there). The feature flag is the
 * project's (include/canary/config.h, FEATURE_IMPROV), which the .cpp reads
 * through __has_include so the one -DFEATURE_IMPROV=0 veto CI applies per
 * board compiles this module and its call sites out together.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "network/improv_core.h"

namespace canary {
namespace net {
namespace improv_ble {

/// What the board tells the phone about itself, and the hooks it offers.
/// Strings must outlive begin().
struct Identity {
  const char* firmware_name;     // "canary-sense"
  const char* firmware_version;  // CANARY_FW_VERSION
  const char* hardware;          // the board, as its pins header spells it
  const char* device_name;       // the device id the mDNS advert carries
  /// The short local name on air while the door is open: the family word
  /// and the device's pseudonym suffix, "Sense-AB12" — the same four
  /// characters as its SecuraCV-XXXX setup network, so a phone can tell
  /// the two doors are one device. At most 12 characters (the advert
  /// budget); longer is cut.
  const char* adv_name;
  /// Where the device can be reached once joined, for the WIFI_SETTINGS
  /// result ("" when it serves no page — Sense and Vision). Returns the
  /// length written. May be null.
  size_t (*reach_url)(char* out, size_t cap);
  /// Blink something for IDENTIFY; null when the board has nothing to blink
  /// (the capability bit follows).
  void (*identify)();
  /// Require an encrypted link for the credentials write (LE Secure
  /// Connections, Just Works — iOS shows its one-tap pairing sheet the first
  /// time). The credentials then never cross the air in the clear.
  bool require_encryption;
};

/**
 * @brief Register the service and start the session.
 *
 * Brings NimBLE up under `identity.device_name` if nothing has yet
 * (the beacon usually has), creates the server when the board has none,
 * registers the five characteristics and starts the server. `no_wifi` is
 * the door's first state (the portal is up). Idempotent; false when the
 * stack is not available this boot (the module then stays a no-op).
 */
bool begin(const Identity& identity, bool no_wifi, uint32_t now_ms);

/**
 * @brief One pass: process a received command, follow the join, time out,
 * republish. `no_wifi` is the door — pass setup_portal_active(). Cheap when
 * idle; call every loop() pass after begin().
 */
void tick(uint32_t now_ms, bool no_wifi);

/// A physical tap on a board that reads one: opens the door for a minute.
void tap(uint32_t now_ms);

/// True once begin() succeeded this boot.
bool active();

/// The session's current state, for logs.
improv::State state();

/// True while the door is open: the beacon carrier sets
/// FLEET_BEACON_FLAG_SETUP_OPEN from this.
bool setup_open();

/**
 * @brief Put the right advert set on air for the current state.
 *
 * `beacon_mfg` is the full manufacturer blob (company id included) the
 * fleet beacon advertises, or null. Door open: beacon + name primary,
 * connectable, Improv UUID + service data in the scan response. Door shut:
 * beacon primary alone, as before. Does the stop → set → start NimBLE
 * needs on both majors. Safe before begin() (beacon only).
 */
void advertise(const uint8_t* beacon_mfg, size_t beacon_len);

}  // namespace improv_ble
}  // namespace net
}  // namespace canary
