/**
 * @file peer_link.h
 * @brief The hub-less mesh on the LAN — the I/O half of peer_protocol.h.
 *
 * One lwIP UDP socket on the fleet-link multicast group and port the presence
 * beacon already uses (239.83.67.86:5867, fleet_beacon_udp.h: TTL 1, never
 * routed off the subnet), bound with SO_REUSEADDR so it coexists with a
 * board's beacon sender or receiver on the same port. Every HEARTBEAT_MS it
 * multicasts an authenticated heartbeat; every pass it drains what the group
 * carried, verifies each frame against the household fleet key
 * (fleet_key.h), drops replays and its own echo, and keeps the peer table.
 * Events go unicast to the coordinator when a peer coordinates, multicast
 * otherwise — one schema either way (peer_protocol.h).
 *
 * ARMING: the link is armed with the network the node is on (the SSID and
 * password the onboarding step was handed): the key is derived from them, or
 * the hub's stored replacement (`fleet_key` in NVS) wins. An OPEN network
 * yields no key, and the link stays DISARMED with one logged warning — the
 * protocol never runs unauthenticated (the brief's explicit degradation; the
 * status body says `peer: "off"`). The board re-arms on every (re)join.
 *
 * What this gives a hub-less house: every node knows every other node is up
 * (`peers_seen`), one of them is the deterministic coordinator (the roll-call
 * aggregator and, in docs/OTA.md, the staged-rollout driver), and the moment
 * any node is on its hub, every node yields to the hub.
 *
 * Trust: a frame verifies against the household key, nothing more — "paired"
 * means "handed this household's Wi-Fi by its owner". docs/TRUST_MODEL.md.
 *
 * Compiled directly by each adopting board's build (build_src_filter), core
 * 2.x and 3.x clean, like the other common/network sources.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "network/peer_protocol.h"

namespace canary {
namespace net {
namespace peer_link {

struct Identity {
  uint8_t fp[peer::FP_BYTES];   ///< this node's pubkey fingerprint (8 bytes)
  uint8_t role;                 ///< peer::ROLE_*
  uint16_t caps;                ///< mdns_txt Cap bits
};

struct Hooks {
  /// Fill this node's heartbeat facts each send. May be null.
  void (*fill_heartbeat)(peer::Heartbeat& out);
  /// Is this node on its hub right now? Drives FLAG_HUB_OK. May be null.
  bool (*hub_ok)();
};

void begin(const Identity& id, const Hooks& hooks);

/// Replace the fingerprint after begin() — for a board whose witness key is
/// established later in setup() than the link (vision's order). Zero bytes
/// until then say "keyless" honestly rather than inventing an identity.
void set_fp(const uint8_t fp[peer::FP_BYTES]);

/// Arm with the network the node is on. False (logged once) when no key can
/// be made: the link stays off. Call on every (re)join.
bool arm(const char* ssid, const char* pass);
/// Link lost: close the socket, keep the table (peers age out by themselves).
void disarm();

/// Every loop pass.
void tick(uint32_t now_ms, bool sta_online);

bool armed();     ///< a key is held
bool ready();     ///< socket open on the current address

const peer::Table& table();
int peers();      ///< live peers in the table

peer::Election election(uint32_t now_ms);
/// The `coord` word for mDNS / status: "hub" | "self" | <fp hex>. Returns length.
size_t coord_word(char* out, size_t cap, uint32_t now_ms);

/// Send an event now: unicast to a coordinating peer, multicast otherwise.
bool send_event(const peer::Event& e, uint32_t now_ms);

/// The 8-hex id of the key in use ("" when disarmed) — for logs and status,
/// never the key.
const char* key_id();

uint32_t rx_frames();
uint32_t tx_frames();
uint32_t rejected();

}  // namespace peer_link
}  // namespace net
}  // namespace canary
