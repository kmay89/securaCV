// src/net/fleet_udp.cpp — fleet-link presence beacon, LAN-multicast carrier.
//
// See canary/net/fleet_udp.h for what this band is for, and
// firmware/common/fleet_link/fleet_beacon_udp.h for the wire contract. This
// file is transmission only: the bytes come from fleet_beacon_payload.cpp,
// identical to the ones the BLE carrier advertises.
//
// Raw lwIP sockets rather than WiFiUDP, for one reason: IP_MULTICAST_TTL. The
// contract promises TTL 1 — a presence beacon must not be routable off the
// local subnet — and that is a privacy property, so it gets SET rather than
// inherited from whatever the stack defaults to. WiFiUDP does not expose the
// descriptor, so it cannot make that promise.

#include "canary/net/fleet_udp.h"

#include "canary/config.h"

#if defined(FEATURE_FLEET_UDP) && FEATURE_FLEET_UDP

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <string.h>
#include <unistd.h>

#include <fleet_beacon_udp.h>   // group / port / TTL contract (common/fleet_link)

#include "canary/net/fleet_beacon_payload.h"
#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

namespace canary::net {

namespace {

int      s_fd        = -1;
uint32_t s_bound_ip  = 0;    // STA address the socket was opened on
uint32_t s_next_ms   = 0;
uint32_t s_last_gen  = 0;
uint32_t s_sent      = 0;
bool     s_announced = false;

void close_sock() {
  if (s_fd >= 0) {
    ::close(s_fd);
    s_fd = -1;
  }
  s_bound_ip = 0;
}

// Open a sending socket bound to the current STA address. Returns false
// quietly on any failure — this band simply stays down for now and the tick
// retries; it must never be the reason a boot fails or a rejoin stalls.
bool open_sock(uint32_t local_ip) {
  close_sock();

  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return false;

  // TTL 1 — the datagram dies at the first router. Contract, not tuning.
  const uint8_t ttl = FLEET_BEACON_UDP_TTL;
  if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
    ::close(fd);
    return false;
  }

  // Send out of the STA interface explicitly. On a device that may also hold
  // a SoftAP up (setup wizard), letting the stack choose could put the beacon
  // on the provisioning AP instead of the home network.
  struct in_addr ifaddr;
  ifaddr.s_addr = local_ip;
  if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) {
    ::close(fd);
    return false;
  }

  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = local_ip;
  local.sin_port = htons(FLEET_BEACON_UDP_PORT);
  if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
    // Binding the well-known port is a nicety (it makes the source port
    // predictable); an ephemeral source port still delivers. Don't fail the
    // band over it.
    local.sin_port = 0;
    if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
      ::close(fd);
      return false;
    }
  }

  s_fd = fd;
  s_bound_ip = local_ip;
  return true;
}

void send_now(uint32_t now) {
  uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
  const size_t n = fleet_beacon_payload_build(mfg);

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(FLEET_BEACON_UDP_PORT);
  dst.sin_addr.s_addr = inet_addr(FLEET_BEACON_UDP_GROUP);

  const int rc = (int)::sendto(s_fd, mfg, n, 0, (struct sockaddr*)&dst,
                               sizeof(dst));
  if (rc < 0) {
    // The link went out from under the socket. Drop it; the next tick opens a
    // fresh one when there is an address to open it on. No error state to
    // clear by hand — that is the whole recovery path.
    close_sock();
    return;
  }

  s_sent++;
  s_last_gen = fleet_beacon_payload_generation();
  s_next_ms = now + FLEET_BEACON_UDP_REFRESH_MS;

  if (!s_announced) {
    s_announced = true;
    canary::log_line("BEACON",
                     "Fleet-link presence beacon on the LAN (multicast, broker-free).");
  }
}

}  // namespace

void fleet_udp_begin(uint32_t /*now*/) {
  // Nothing to do up front. The socket needs an STA address, which may not
  // exist yet and may change later, so it is opened by the tick instead —
  // one code path for "first join" and "rejoined on a new address".
}

void fleet_udp_tick(uint32_t now) {
  if (!canary::net::wifi_connected()) {
    // No link: make sure we are not holding a socket bound to an address that
    // no longer exists. Nothing else to unwind.
    close_sock();
    return;
  }

  const uint32_t ip = (uint32_t)WiFi.localIP();
  if (ip == 0) return;                    // associated but no address yet

  // Reopen on a new address (DHCP renewal onto a different lease, a move to
  // another AP). Comparing the bound address is what makes that automatic.
  if (s_fd < 0 || ip != s_bound_ip) {
    if (!open_sock(ip)) return;           // retry next tick
    s_next_ms = now;                      // announce ourselves promptly
  }

  // A detection edge goes out immediately; otherwise ride the refresh.
  if (fleet_beacon_payload_generation() != s_last_gen) {
    send_now(now);
    return;
  }
  if ((int32_t)(now - s_next_ms) < 0) return;
  send_now(now);
}

uint32_t fleet_udp_sent() { return s_sent; }

}  // namespace canary::net

#else  // FEATURE_FLEET_UDP off — no-op stubs (per-board size-guard off switch)

namespace canary::net {
void fleet_udp_begin(uint32_t) {}
void fleet_udp_tick(uint32_t) {}
uint32_t fleet_udp_sent() { return 0; }
}  // namespace canary::net

#endif  // FEATURE_FLEET_UDP
