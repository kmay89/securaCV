/*
 * SecuraCV Canary — Fleet-Link Presence Beacon over ESP-NOW (transport)
 *
 * The router-free band for the beacon defined in fleet_beacon.h. The three
 * carriers, side by side:
 *
 *   BLE advert    — no WiFi at all, direct radio range, needs the BLE stack.
 *   ESP-NOW (this file) — no router, no association, no credentials: raw
 *                   802.11 action frames between two ESP32s that may never
 *                   have seen a network in their lives. This is the band a
 *                   boxed pair works on out of the box.
 *   UDP multicast — rides the home WiFi both devices already joined, the
 *                   only band that reaches across a house.
 *
 * ── THE PAYLOAD IS THE BEACON, VERBATIM ────────────────────────────────────
 * The ESP-NOW frame body is the SAME manufacturer-data blob the BLE advert
 * carries and the UDP datagram wraps — 11 bytes for v1, 13 for v2, including
 * the two leading company-id bytes. No ESP-NOW-specific framing, no header,
 * no envelope. The receiver hands the body to the same shared decode every
 * band uses (fleet_beacon_parse() / beacon_frame.h), so a field added to the
 * beacon reaches every transport at once and no transport can quietly speak a
 * dialect. canary-display's espnow_peer.cpp has decoded exactly this shape
 * since the receive side was written; this header names the send-side
 * constants so a transmitter and that receiver agree by construction.
 *
 * ── WHAT IS ON THE WIRE ────────────────────────────────────────────────────
 * Exactly what the BLE advert already broadcasts in the clear: flags,
 * battery/health percentages, a chain height, two fingerprint bytes, and on
 * v2 a class token and a confidence percentage. No identity, no image, no
 * audio, no timestamp — the frame IS the "now" (Invariant II bounds the
 * vocabulary to the ObjectClass enum; a face or a plate here is a rejected
 * PR, not a config flag). Broadcast to the all-ones MAC: nothing is paired,
 * nothing is encrypted, and nothing needs to be — this is the presence hint
 * channel, and trust never rises above presence on it.
 *
 * ── CHANNEL POLICY ─────────────────────────────────────────────────────────
 * The ESP32 has ONE 2.4 GHz radio, so ESP-NOW rides whatever channel the
 * WiFi MAC is tuned to. The policy here is the mesh's proven one
 * (canary-wap mesh_channel_policy.h), applied to a transmitter:
 *
 *   STA associated      → send on the STA channel (the radio is already
 *                         there, and fighting it degrades the home network —
 *                         the exact failure mesh_channel_policy exists for).
 *   STA configured but  → send on whatever channel the reconnect machinery
 *   temporarily down      has the radio on; never retune under it.
 *   never provisioned   → park on FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL so a
 *                         factory-fresh pair finds each other with zero
 *                         setup. Matches MESH_FALLBACK_CHANNEL (6) so every
 *                         router-free SecuraCV radio idles on one channel.
 *
 * This header is PURE: no esp_now.h, no Arduino — just the contract, so the
 * constants can be host-tested and included by RX-side policy code.
 */

#ifndef FLEET_BEACON_ESPNOW_H
#define FLEET_BEACON_ESPNOW_H

#include <stdint.h>
#include <stddef.h>

#include "fleet_beacon.h"

// ════════════════════════════════════════════════════════════════
// TRANSPORT CONSTANTS
// ════════════════════════════════════════════════════════════════

// Broadcast destination: the all-ones MAC. A presence beacon is addressed to
// whoever can hear it, exactly like the BLE advert — adding a unicast peer
// would invent a pairing this band deliberately does not have.
#define FLEET_BEACON_ESPNOW_BCAST_ADDR \
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

// Unsolicited refresh cadence. Matches the BLE advert and the UDP carrier
// (5 s) so every band carries the same freshness; a DETECTION EDGE sends
// immediately and does not wait for this.
#define FLEET_BEACON_ESPNOW_REFRESH_MS 5000

// The channel a never-provisioned radio parks on so two boxed devices meet
// with zero configuration. The same value as the mesh's
// MESH_FALLBACK_CHANNEL (canary-wap mesh_channel_policy.h) — mid-band,
// non-overlapping — and it must stay agreed with it: every router-free
// SecuraCV radio idling on one channel is what makes "power both and wave"
// work.
#define FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL 6

// Largest frame body worth reading — a v2 blob. Receivers read at most this
// many bytes; the shared parser rejects anything that disagrees with its own
// length/version bytes.
#define FLEET_BEACON_ESPNOW_MAX_LEN FLEET_BEACON_MFG_V2_LEN

// ════════════════════════════════════════════════════════════════
// PURE ACCEPT PREDICATE
// ════════════════════════════════════════════════════════════════

// Decide whether a received ESP-NOW frame body is a fleet-link presence
// beacon and, if so, parse it — the same decision, and the same bounds, as
// fleet_beacon_udp_accept(). Returns false and writes nothing on rejection.
static inline bool fleet_beacon_espnow_accept(const uint8_t* body, size_t n,
                                              FleetBeaconFields* out) {
    if (!body || !out) return false;
    if (n > FLEET_BEACON_ESPNOW_MAX_LEN) return false;
    return fleet_beacon_parse(body, n, out);
}

#endif // FLEET_BEACON_ESPNOW_H
