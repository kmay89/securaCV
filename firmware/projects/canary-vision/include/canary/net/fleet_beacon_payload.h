#pragma once
#include <stddef.h>
#include <stdint.h>

#include <fleet_beacon.h>   // FLEET_BEACON_MFG_V2_LEN, FLEET_BEACON_DETECT_*

// Fleet-link presence beacon — the PAYLOAD, with no radio attached.
//
// This module owns what the beacon SAYS. The modules that own how it travels
// are carriers, and there are two of them today:
//
//   fleet_beacon_adv.h  — BLE advert     (no WiFi at all)
//   fleet_udp.h         — LAN multicast  (no broker, reaches across a house)
//
// Splitting it this way is the whole answer to "how do we do all the bands
// without duplication": the bytes are built ONCE, here, and every carrier
// transmits that same buffer verbatim. A carrier cannot compose its own
// variant of the beacon — it has no builder — so the transports are physically
// incapable of drifting into dialects, and a new field reaches every band the
// day it is added. It is also why this module is NOT behind
// FEATURE_FLEET_BEACON: turning the BLE radio off for flash budget must not
// take the payload (and therefore the WiFi carrier) down with it.

namespace canary::net {

// Build the current beacon as the full on-air manufacturer blob — the two
// company-id bytes followed by the v2 payload, exactly the shape a BLE scanner
// sees and exactly the bytes a UDP carrier puts in a datagram. Returns the
// length written (FLEET_BEACON_MFG_V2_LEN).
//
// Honest sourcing, no fabricated fields: degraded comes from the diagnostics
// level, on_wifi_sta from the real STA link, alert/class/score from the
// presence FSM below, chain height from the witness chain, and the fingerprint
// suffix from the witness identity once it is ready (0x00 until then — never
// invented). Battery and health are the 0xFF unknown sentinel: this is a
// mains-powered witness with no gauge and no 0..100 self-test at this layer.
size_t fleet_beacon_payload_build(uint8_t out[FLEET_BEACON_MFG_V2_LEN]);

// Bumped whenever the payload changes in a way a peer should hear NOW rather
// than at the next refresh — presence flipping, or the class changing while
// presence holds. Carriers poll this and send immediately when it moves, which
// is how a detection edge reaches every band at once without this module
// knowing a single thing about radios.
uint32_t fleet_beacon_payload_generation();

// Feed the live detection state in. Cheap to call every vision tick: a mere
// confidence wobble rides the next refresh, while an EDGE bumps the generation
// so carriers republish at once. detect_class is a FLEET_BEACON_DETECT_* token
// — a class and a percentage, never anything identifying.
void fleet_beacon_note_detection(bool active, uint8_t detect_class,
                                 int score_pct, uint32_t now);

// When the generation last bumped (the `now` a carrier's edge republish
// measures its trigger timing against). Local instrumentation only: the wire
// contract deliberately carries no timestamp — the frame IS the "now" — so
// this number is for this device's own serial log, never for the beacon.
uint32_t fleet_beacon_payload_edge_ms();

}  // namespace canary::net
