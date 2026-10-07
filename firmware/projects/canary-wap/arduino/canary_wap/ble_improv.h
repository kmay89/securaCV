/*
 * SecuraCV Canary WAP — the Bluetooth setup door (Improv Wi-Fi over BLE)
 *
 * The open Improv Wi-Fi standard, on the pairing channel's NimBLE server,
 * for the moment a fresh WAP has no Wi-Fi of its own: a phone hears it
 * (Opera's beacon carries FLEET_BEACON_FLAG_SETUP_OPEN and the name
 * "WAP-XXXX"; the scan response carries the Improv service), shows a card,
 * and one tap hands over the home Wi-Fi. The bytes and the rules are in
 * improv_core.h (a staged copy of firmware/common/network/improv_core.h,
 * held byte-identical by firmware/scripts/check_improv_sync.sh); this
 * module is the sketch's glue.
 *
 * WHAT IS DIFFERENT FROM ble_provision (the bonded rescue service):
 *   - ble_provision needs an authenticated bond (the dashboard's Numeric
 *     Comparison); this door needs an ENCRYPTED link, nothing more — LE
 *     Secure Connections Just Works, no bond kept, so a phone with no
 *     dashboard open can still use it. The channel swaps its security
 *     profile to that while the door is open (bluetooth_channel::
 *     set_setup_door) and back when it shuts; the key such a pairing
 *     yields is unauthenticated, so the console, OTA, witness-export and
 *     ble_provision characteristics (READ/WRITE_AUTHEN) still refuse it.
 *   - The door is open only while the unit has NO credentials stored
 *     (first boot, factory reset). A WAP on its own Wi-Fi offers nothing
 *     here: its owner's paths are the dashboard, the bonded rescue, and
 *     the BOOT-tap receipt.
 *   - The pairing receipt rides the same link: once the join the phone
 *     asked for succeeded, the connection that asked may read RECEIPT
 *     (8fc1cf01, READ_ENC) exactly once — the same JSON the BOOT-tap route
 *     serves, with base_url pointing at the device's .local name rather
 *     than the SoftAP address (which is gone once the AP drops). So the
 *     phone ends up PAIRED, token in hand, from one tap.
 *
 * Compiled under FEATURE_IMPROV (build_config.h: the FULL profile), with
 * no-op stubs otherwise.
 */

#ifndef SECURACV_BLE_IMPROV_H
#define SECURACV_BLE_IMPROV_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;
class NimBLEAdvertisementData;

namespace ble_improv {

/// SecuraCV's one addition inside the Improv service: the pairing receipt,
/// readable once by the link that provisioned, encrypted.
static constexpr const char* RECEIPT_UUID = "8fc1cf01-b162-4401-9607-c8ac21383e90";

/// Register the service on the channel's server (after ble_provision::init).
bool init(NimBLEServer* server);

/// Loop-task pass: follow the door, the join, the receipt arming; swap the
/// channel's security profile with the door. Called from
/// bluetooth_channel::update() beside ble_provision::tick().
void tick();

/// True while the door is open — Opera sets the beacon's setup bit and the
/// "WAP-XXXX" name from this, and composes the scan response below.
bool setup_open();

/// The local name on air while the door is open ("WAP-XXXX"), empty before init.
const char* adv_name();

/// Fill the scan response for an open door: the Improv service UUID and
/// its 0x4677 service data (28 bytes).
void compose_scan_response(NimBLEAdvertisementData& scan);

struct Stats {
  uint32_t credentials_accepted;
  uint32_t credentials_refused;
  uint32_t receipts_served;
};
bool get_stats(Stats* out);

}  // namespace ble_improv

#endif  // SECURACV_BLE_IMPROV_H
