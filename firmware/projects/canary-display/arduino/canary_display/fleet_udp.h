// Fleet-link presence beacon over LAN multicast — the DISPLAY side (receive).
//
// The third band this display listens on, and the only one that reaches past
// direct radio range. The other two:
//
//   chirp_scan.h    — BLE advert   (works with no WiFi at all)
//   espnow_peer.h   — ESP-NOW      (works with no router at all)
//   this file       — LAN multicast (works across a house, still no broker)
//
// All three deliver the SAME beacon bytes into the SAME parser
// (canary/net/beacon_parse.h) and the SAME ingest (FleetModel::on_beacon), and
// a witness is identified by its fingerprint suffix rather than by the radio
// that carried it. So hearing one Canary on two bands adds coverage, never a
// duplicate device and never a duplicate alert — the model's dedupe windows
// are deliberately band-independent. Adding a band here costs nothing but the
// band.
//
// Honesty rule, same as ESP-NOW: the display has no signing identity, so it
// never transmits — it is a receive-only observer of peer liveness. Multicast
// carries only the coarse presence summary the beacon already coarsens; a
// peer's signed chain head still travels over the signed pipeline, and nothing
// arriving here can touch a trust badge.
//
// Gated by FEATURE_FLEET_UDP (canary/config.h, default 1). When 0 these become
// no-ops so a board's size guard can veto the band without touching call sites.

#pragma once

#include <cstdint>

namespace canary {
namespace net {

// Join the beacon multicast group on the current STA interface. Safe to call
// before WiFi is up — the socket is opened lazily by the loop once there is a
// link, so first join and rejoin are one code path. Idempotent.
void fleet_udp_begin(uint32_t now);

// Drain received datagrams into the fleet model. Cheap, non-blocking and
// self-throttling; safe to call every loop pass.
//
// Self-healing: the socket is dropped when the link goes and reopened on the
// address that comes back, so a router reboot, a DHCP renewal onto a new
// lease, or a move to another AP all recover without anything to reset. There
// is no "joined" flag that can survive the interface it described.
void fleet_udp_loop(uint32_t now);

// True while a socket is open and joined to the group.
bool fleet_udp_ready();

// Well-formed beacons received this session (diagnostics). Datagrams that fail
// the wire check are rejected before the fleet model and are not counted.
uint32_t fleet_udp_seen();

}  // namespace net
}  // namespace canary
