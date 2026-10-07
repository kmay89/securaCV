/*
 * SecuraCV Canary WAP — the Bluetooth setup door (Improv Wi-Fi over BLE)
 *
 * The open Improv Wi-Fi standard, on the pairing channel's NimBLE server,
 * for a WAP nobody owns yet: while no credentials are stored, for the
 * first-boot window (IMPROV_FIRST_BOOT_WINDOW_MS after the last POWER
 * CYCLE — a software reset carries the spent time over, see below), a
 * phone hears it (Opera's beacon carries FLEET_BEACON_FLAG_SETUP_OPEN and
 * the name "WAP-XXXX"; the scan response carries the Improv service),
 * shows a card, lists the networks the WAP can see (GET_WIFI_NETWORKS, the
 * same scan cache the wizard's picker reads), and one tap hands over the
 * home Wi-Fi. The bytes and the rules are in improv_core.h (a staged copy
 * of firmware/common/network/improv_core.h, held byte-identical by
 * firmware/scripts/check_improv_sync.sh); this module is the sketch's glue.
 *
 * THE WINDOW SURVIVES A REBOOT, NOT A POWER CYCLE. A never-provisioned WAP
 * restarts itself every 15 minutes (setup_wizard's abandonment timer), and
 * a window measured from each boot re-opened the door forever. The sketch
 * keeps the spent time in an RTC-noinit record (ble_improv_window_used_ms /
 * ble_improv_note_window_used, written every ~2 s while the door's window
 * runs; zeroed after a power-on, a brownout, a power glitch or a reset a
 * host asserted from the USB / JTAG port — the Flasher's — and by a
 * credential wipe on a unit that had credentials),
 * and init() hands it to improv::session_begin, which back-dates the
 * window: a window the last boots spent whole begins shut. And a phone at
 * the door is a sign of life for that wizard timer (ble_improv_note_
 * activity on every command, the join and the claim), so the restart
 * cannot land mid-provisioning.
 *
 * WHAT IS DIFFERENT FROM ble_provision (the bonded rescue service):
 *   - ble_provision needs an authenticated bond (the dashboard's Numeric
 *     Comparison); this door needs an ENCRYPTED link, nothing more — LE
 *     Secure Connections Just Works, no bond kept, so a phone with no
 *     dashboard open can still use it. The channel swaps its security
 *     profile to that while the door is open (bluetooth_channel::
 *     set_setup_door, deciding through provisioning_logic::
 *     security_profile_for: never under a pending pairing, never under an
 *     authenticated or bonded link) and back when it shuts; the key such a
 *     pairing yields is unauthenticated, so the console, OTA, witness-export
 *     and ble_provision characteristics (READ/WRITE_AUTHEN) still refuse
 *     it. Encryption is checked twice: by the command characteristic's
 *     WRITE_ENC property, and again in the write handler, which refuses a
 *     write from an unencrypted link NotAuthorized whatever the stack let
 *     through.
 *   - The door is open only while NO credentials are stored (first boot,
 *     factory reset), and only for the first-boot window; a saved network
 *     that stopped working raises the SoftAP recovery portal (a door with a
 *     printed key), never this one. A WAP on its own Wi-Fi offers nothing
 *     here: its owner's paths are the dashboard, the bonded rescue, and
 *     the BOOT-tap receipt. Accepted credential writes are rate-limited and
 *     capped per open door, and a client that sends nothing is dropped
 *     from the single link (improv_core.h Timing).
 *   - THE BEARER TOKEN NEVER RIDES BLUETOOTH. The link is encrypted but not
 *     authenticated, so nothing that lasts belongs on it. Once the join the
 *     phone asked for succeeded, the connection that asked may read CLAIM
 *     (the SecuraCV companion service below, READ_ENC): a 16-byte random
 *     claim ticket (claim_ticket.h, a staged copy of
 *     firmware/common/network/claim_ticket.h), the URL to spend it at, the
 *     device's .local name, its STA address and TLS fingerprint. The claim
 *     is armed BEFORE the WIFI_SETTINGS result and the Provisioned state
 *     go out, so a phone that reads it the moment it hears the verdict
 *     finds it. It is readable by the provisioning link alone, during one
 *     read — the value is longer than an MTU, so that read is a Read
 *     Response plus its Read Blob continuations — within a 3 s grace from
 *     the first; empty ("{}") afterwards, and for anyone else at any time.
 *     The phone then spends the claim on the home LAN —
 *     GET /api/provisioning-receipt?claim=<hex> — and gets the same receipt
 *     the BOOT-tap route serves. Two factors: the encrypted link that
 *     provisioned AND presence on the Wi-Fi the device just joined. The
 *     claim is good for three minutes, once (the sketch holds its
 *     post-provisioning reboot while it is outstanding).
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

/// SecuraCV's companion service beside the Improv one: the claim ticket,
/// readable by the link that provisioned during one read (its blob
/// continuations, within a 3 s grace), encrypted; "{}" otherwise.
/// Registered in init() on the same server as the Improv service.
static constexpr const char* CLAIM_SERVICE_UUID = "8fc1cf00-b162-4401-9607-c8ac21383e90";
static constexpr const char* CLAIM_UUID         = "8fc1cf01-b162-4401-9607-c8ac21383e90";

/// Register the Improv service and the companion (claim) service on the
/// channel's server (after ble_provision::init).
bool init(NimBLEServer* server);

/// Loop-task pass: follow the door, the one link (idle disconnect), the
/// join, the claim arming, the network list, the window tally; swap the
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
  uint32_t credentials_refused;   // bad frames, unencrypted writes, a shut door
  uint32_t claims_served;         // claim tickets read over the link that provisioned
};
bool get_stats(Stats* out);

}  // namespace ble_improv

#endif  // SECURACV_BLE_IMPROV_H
