// LAN-multicast presence-beacon receiver — see canary/net/fleet_udp.h.
//
// The band that reaches across a house. This file is transport only: it pulls
// datagrams off a multicast socket and hands the bodies to the shared decode
// (canary/net/beacon_frame.h), exactly as espnow_peer.cpp does with ESP-NOW
// frames and chirp_scan.cpp does with BLE adverts. One wire contract, one
// parser, one ingest — so a Canary heard on this band lands in the same
// witness slot the other bands already use.
//
// Raw lwIP sockets rather than WiFiUDP for the same reason the sender uses
// them: the group membership has to be set on a specific interface and torn
// down with the link, and WiFiUDP does not expose the descriptor needed to do
// that honestly.

#include "fleet_udp.h"

// The composition header, not the flavor <config.h>: FEATURE_FLEET_UDP's
// default-on lives there (flavor configs carry board pins, not band policy).
#include "config.h"

#if defined(FEATURE_FLEET_UDP) && FEATURE_FLEET_UDP

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "fleet_beacon_udp.h"   // group / port / max-len contract

#include "beacon_frame.h"       // shared decode (all bands)
#include "fleet_instance.h"
#include "log.h"

namespace canary {
namespace net {

namespace {

int      s_fd       = -1;
uint32_t s_joined_ip = 0;   // STA address the membership was taken on
uint32_t s_seen     = 0;
bool     s_announced = false;

// Never drain more than this many datagrams in one pass. The group is
// low-rate by design (a beacon every ~5 s per Canary), so this is a bound on
// pathological traffic, not on normal fleets: it keeps a chatty or hostile
// sender from holding the UI thread inside this loop.
constexpr int MAX_DRAIN_PER_PASS = 8;

void close_sock() {
  if (s_fd >= 0) {
    ::close(s_fd);
    s_fd = -1;
  }
  s_joined_ip = 0;
}

// Open a non-blocking socket bound to the beacon port and join the group on
// the current STA interface. Returns false quietly on failure — the band stays
// down and the next pass retries. It must never be the reason a boot fails.
bool open_sock(uint32_t local_ip) {
  close_sock();

  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return false;

  // Several listeners may share the port (and a re-open after a link flap must
  // not trip over the old binding).
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);   // group traffic, any source
  local.sin_port = htons(FLEET_BEACON_UDP_PORT);
  if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
    ::close(fd);
    return false;
  }

  // Join on the STA interface specifically. On a device that may also hold a
  // SoftAP up (setup wizard), letting the stack choose could join the group on
  // the provisioning AP and hear nothing from the house.
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(FLEET_BEACON_UDP_GROUP);
  mreq.imr_interface.s_addr = local_ip;
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    ::close(fd);
    return false;
  }

  // Non-blocking: the drain below must never park the UI thread waiting for a
  // datagram that isn't coming.
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  s_fd = fd;
  s_joined_ip = local_ip;
  return true;
}

}  // namespace

void fleet_udp_begin(uint32_t /*now*/) {
  // Nothing up front: the membership needs an STA address, which may not exist
  // yet and may change later, so the loop opens it. One path for join, rejoin,
  // and a new lease.
}

bool fleet_udp_ready() { return s_fd >= 0; }
uint32_t fleet_udp_seen() { return s_seen; }

void fleet_udp_loop(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    // The interface the membership was taken on is gone. Drop the socket —
    // holding it would leave us "joined" to a group on an address that no
    // longer exists, which is exactly the stale state that never recovers.
    close_sock();
    return;
  }

  const uint32_t ip = (uint32_t)WiFi.localIP();
  if (ip == 0) return;                    // associated, no address yet

  if (s_fd < 0 || ip != s_joined_ip) {
    if (!open_sock(ip)) return;           // retry next pass
    if (!s_announced) {
      s_announced = true;
      log_line("FLEET-UDP",
               "Listening for presence beacons on the LAN (multicast, broker-free).");
    }
  }

  for (int i = 0; i < MAX_DRAIN_PER_PASS; i++) {
    // Read at most a v2 blob. A larger datagram is not ours, and capping the
    // read means an oversize one is truncated here and then rejected by the
    // length check in the decode rather than sized by its sender.
    uint8_t buf[FLEET_BEACON_UDP_MAX_LEN];
    const int n = (int)::recv(s_fd, buf, sizeof(buf), 0);
    if (n <= 0) break;                    // drained (or the socket went away)

    char fp4[5];
    canary::fleet::BeaconStatus st;
    bool have_status = false;
    if (!beacon_frame_decode(buf, n, fp4, st, have_status)) continue;

    s_seen++;
    // Unsigned observation, same footing as the BLE and ESP-NOW beacons. The
    // fleet model does the identity match and the band-independent dedupe, so
    // hearing a Canary here that is also audible on BLE costs one refreshed
    // timestamp — not a second witness and not a second alert.
    canary::fleet::the_fleet().on_beacon(fp4, st, have_status, now,
                                         canary::fleet::Via::Wifi);
  }
}

}  // namespace net
}  // namespace canary

#else  // FEATURE_FLEET_UDP off — no-op stubs (per-board size-guard off switch)

namespace canary {
namespace net {
void fleet_udp_begin(uint32_t) {}
void fleet_udp_loop(uint32_t) {}
bool fleet_udp_ready() { return false; }
uint32_t fleet_udp_seen() { return 0; }
}  // namespace net
}  // namespace canary

#endif  // FEATURE_FLEET_UDP
