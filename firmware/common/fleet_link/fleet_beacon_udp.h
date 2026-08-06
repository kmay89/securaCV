/*
 * SecuraCV Canary — Fleet-Link Presence Beacon over LAN multicast (transport)
 *
 * The THIRD transport for the beacon defined in fleet_beacon.h, and the only
 * one that reaches across a house. The other two:
 *
 *   BLE advert   — no WiFi at all, but only as far as the radio carries.
 *   ESP-NOW      — no router at all, but still device-to-device range.
 *   UDP multicast (this file) — rides the home WiFi the devices are ALREADY
 *                  joined to, so a Vision downstairs reaches a Display
 *                  upstairs. Still NO broker, NO hub, NO pairing, NO cloud.
 *
 * "Over WiFi" here means over the LAN the user already has. It does NOT mean
 * a server: a multicast datagram is addressed to a group, not to a host, so
 * nothing has to be discovered, configured, logged into, or kept running. A
 * Vision that is joined to WiFi starts sending; a Display that is joined to
 * the same WiFi starts hearing. That is the whole setup.
 *
 * ── THE PAYLOAD IS THE BEACON, VERBATIM ────────────────────────────────────
 * The datagram body is the SAME manufacturer-data blob the BLE advert carries,
 * INCLUDING the two leading company-id bytes that NimBLE prepends on air —
 * 11 bytes for v1, 13 for v2. There is no UDP-specific framing, no header, no
 * envelope. That is deliberate and load-bearing: the receiver hands the body
 * straight to the same fleet_beacon_parse() / beacon_parse.h the BLE and
 * ESP-NOW paths use, so all three transports resolve to one witness through
 * one host-tested parser. A field added to the beacon reaches every transport
 * at once, and no transport can quietly understand a dialect of it.
 *
 * (ESP-NOW already works exactly this way — see canary-display's
 * espnow_peer.cpp. This is that pattern, over IP.)
 *
 * ── WHAT IS ON THE WIRE ────────────────────────────────────────────────────
 * Nothing that isn't already broadcast in the clear over BLE to anyone in
 * radio range: flags, battery/health percentages, a chain height, two
 * fingerprint bytes, and — on v2 — a class token and a confidence percentage.
 * No identity, no image, no audio, no timestamp (Invariant II: the vocabulary
 * is the ObjectClass enum; a face or a plate here is a rejected PR, not a
 * config flag). This transport is strictly LESS exposed than the BLE advert
 * it mirrors: TTL 1 keeps every datagram inside the local subnet, so it
 * cannot be routed off the LAN, and unlike a broker there is no third party
 * that receives, stores, or forwards any of it.
 *
 * ── GROUP AND PORT ─────────────────────────────────────────────────────────
 * 239.83.67.86 is in 239.0.0.0/8, the RFC 2365 Organization-Local Scope set
 * aside for private use (the multicast analogue of RFC 1918). The last three
 * octets are 'S','C','V'. The port is not IANA-registered; it is ours by
 * convention, and the type/version bytes inside the payload — not the port —
 * are what actually decide whether a datagram is one of ours.
 *
 * This header is PURE: no lwIP, no Arduino, no sockets — just the contract and
 * a host-testable accept predicate, so test_fleet_beacon_udp.cpp can exercise
 * the decision a receiver makes without a network stack.
 */

#ifndef FLEET_BEACON_UDP_H
#define FLEET_BEACON_UDP_H

#include <stdint.h>
#include <stddef.h>

#include "fleet_beacon.h"

// ════════════════════════════════════════════════════════════════
// TRANSPORT CONSTANTS
// ════════════════════════════════════════════════════════════════

// Organization-local scope multicast group (RFC 2365), octets 'S','C','V'.
#define FLEET_BEACON_UDP_GROUP    "239.83.67.86"
#define FLEET_BEACON_UDP_GROUP_B0 239
#define FLEET_BEACON_UDP_GROUP_B1 83
#define FLEET_BEACON_UDP_GROUP_B2 67
#define FLEET_BEACON_UDP_GROUP_B3 86

// Destination port. Senders also bind this port so a receiver's socket sees
// group traffic regardless of the sender's ephemeral source port.
#define FLEET_BEACON_UDP_PORT     5867

// TTL 1: a beacon must never leave the local subnet. This is a privacy
// property, not a tuning knob — raising it would let a presence beacon be
// routed somewhere the user never agreed to.
#define FLEET_BEACON_UDP_TTL      1

// Unsolicited refresh cadence. Matches the BLE advert's 5 s rebuild so the two
// transports carry the same freshness; a DETECTION EDGE sends immediately and
// does not wait for this.
#define FLEET_BEACON_UDP_REFRESH_MS 5000

// Largest datagram body worth reading — a v2 blob. A receiver reads at most
// this many bytes and rejects anything longer, so a hostile or confused sender
// can't push a large read through the parser.
#define FLEET_BEACON_UDP_MAX_LEN  FLEET_BEACON_MFG_V2_LEN

// ════════════════════════════════════════════════════════════════
// PURE ACCEPT PREDICATE
// ════════════════════════════════════════════════════════════════

// Decide whether a received datagram body is a fleet-link presence beacon and,
// if so, parse it — the whole receive-side decision, with no socket in sight.
//
// Accepts exactly what the other transports accept: an 11-byte v1 or 13-byte
// v2 blob whose company id, type and version all agree (fleet_beacon_parse
// enforces every one of those). Everything else on the group — a stray
// datagram, a truncated read, another protocol that picked the same port, a
// future version this build predates — is rejected here, before anything
// reaches the fleet model. Returns false and writes nothing on rejection.
static inline bool fleet_beacon_udp_accept(const uint8_t* body, size_t n,
                                           FleetBeaconFields* out) {
    if (!body || !out) return false;
    if (n > FLEET_BEACON_UDP_MAX_LEN) return false;
    return fleet_beacon_parse(body, n, out);
}

#endif // FLEET_BEACON_UDP_H
