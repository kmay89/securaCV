// common/network/peer_link.cpp — the hub-less mesh's socket. See peer_link.h.
//
// Raw lwIP sockets rather than WiFiUDP for the same reason the beacon
// carriers use them: IP_MULTICAST_TTL must be SET to 1 (a privacy property,
// not a default to inherit), and the descriptor is needed for SO_REUSEADDR and
// O_NONBLOCK. Core 2.x and 3.x clean.

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "network/peer_link.h"
#include "network/fleet_key.h"
#include "fleet_link/fleet_beacon_udp.h"   // the group / port / TTL contract

namespace canary {
namespace net {
namespace peer_link {

namespace {

Identity s_id{};
Hooks s_hooks{};
bool s_begun = false;
bool s_armed = false;
bool s_warned_open = false;
uint8_t s_key[fleet_key::KEY_BYTES] = {0};
char s_key_id[9] = {0};

int s_fd = -1;
uint32_t s_bound_ip = 0;
uint32_t s_next_hb_ms = 0;
uint32_t s_seq = 0;
uint32_t s_rx = 0, s_tx = 0, s_rejected = 0;
bool s_announced = false;

peer::Table s_table;

constexpr int MAX_DRAIN_PER_PASS = 8;

void close_sock() {
  if (s_fd >= 0) {
    ::close(s_fd);
    s_fd = -1;
  }
  s_bound_ip = 0;
}

// One socket for both directions: bound to the group port (shared), joined on
// the STA interface, TTL 1, sending from that interface, non-blocking.
bool open_sock(uint32_t local_ip) {
  close_sock();
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return false;
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  const uint8_t ttl = FLEET_BEACON_UDP_TTL;
  if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) { ::close(fd); return false; }
  struct in_addr ifaddr;
  ifaddr.s_addr = local_ip;
  if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0) { ::close(fd); return false; }
  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(FLEET_BEACON_UDP_PORT);
  if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) { ::close(fd); return false; }
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(FLEET_BEACON_UDP_GROUP);
  mreq.imr_interface.s_addr = local_ip;
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) { ::close(fd); return false; }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  s_fd = fd;
  s_bound_ip = local_ip;
  return true;
}

bool send_to(const uint8_t* buf, size_t n, uint32_t ip_be) {
  if (s_fd < 0) return false;
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = ip_be ? ip_be : inet_addr(FLEET_BEACON_UDP_GROUP);
  dst.sin_port = htons(FLEET_BEACON_UDP_PORT);
  const int rc = (int)::sendto(s_fd, buf, n, 0, (struct sockaddr*)&dst, sizeof(dst));
  if (rc < 0) {
    close_sock();  // the link went out from under the socket; the next tick reopens
    return false;
  }
  s_tx++;
  return true;
}

void stamp(peer::Frame& f, uint32_t now_ms) {
  memcpy(f.fp, s_id.fp, peer::FP_BYTES);
  f.role = s_id.role;
  f.uptime_s = now_ms / 1000u;
  f.seq = ++s_seq;
  f.flags = (s_hooks.hub_ok && s_hooks.hub_ok()) ? peer::FLAG_HUB_OK : 0;
  const peer::Election e = peer::elect(s_table, s_id.fp, f.uptime_s, (f.flags & peer::FLAG_HUB_OK) != 0, now_ms);
  if (e.who == peer::Election::Who::Hub) memset(f.coord_fp, 0, peer::FP_BYTES);
  else memcpy(f.coord_fp, e.fp, peer::FP_BYTES);
}

void send_heartbeat(uint32_t now_ms) {
  peer::Frame f;
  peer::Heartbeat hb;
  if (s_hooks.fill_heartbeat) s_hooks.fill_heartbeat(hb);
  hb.caps = s_id.caps;
  hb.peers_seen = (uint8_t)peer::count(s_table);
  peer::put_heartbeat(f, hb);
  stamp(f, now_ms);
  uint8_t buf[peer::MAX_FRAME];
  const size_t n = peer::encode(f, s_key, buf, sizeof(buf));
  if (n) send_to(buf, n, 0);
}

void drain(uint32_t now_ms) {
  for (int i = 0; i < MAX_DRAIN_PER_PASS && s_fd >= 0; i++) {
    uint8_t buf[peer::MAX_FRAME + 1];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    const int n = (int)::recvfrom(s_fd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
    if (n <= 0) break;
    // The presence beacon shares this group: its 11/13 bytes are shorter than
    // any frame and carry no magic, and decode() says so without a log line.
    if ((size_t)n < peer::MIN_FRAME || buf[0] != 'S' || buf[1] != 'C' || buf[2] != 'V' || buf[3] != 'P') continue;
    peer::Frame f;
    const peer::Verdict v = peer::decode(buf, (size_t)n, s_key, f);
    if (v != peer::Verdict::Ok) { s_rejected++; continue; }
    const peer::Verdict a = peer::accept(s_table, f, now_ms, from.sin_addr.s_addr, s_id.fp);
    if (a == peer::Verdict::Self) continue;
    if (a != peer::Verdict::Ok) { s_rejected++; continue; }
    s_rx++;
  }
}

bool load_override(uint8_t out[fleet_key::KEY_BYTES]) {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/true)) return false;
  const size_t n = prefs.getBytes(fleet_key::NVS_KEY, out, fleet_key::KEY_BYTES);
  prefs.end();
  return n == fleet_key::KEY_BYTES;
}

}  // namespace

void begin(const Identity& id, const Hooks& hooks) {
  s_id = id;
  s_hooks = hooks;
  s_begun = true;
  s_table = peer::Table{};
}

void set_fp(const uint8_t fp[peer::FP_BYTES]) {
  if (fp == nullptr) return;
  memcpy(s_id.fp, fp, peer::FP_BYTES);
}

bool arm(const char* ssid, const char* pass) {
  if (!s_begun) return false;
  uint8_t key[fleet_key::KEY_BYTES];
  bool have = load_override(key);
  const char* how = "the hub's key";
  if (!have) {
    have = fleet_key::derive_from_wifi(ssid, pass, key);
    how = "the household Wi-Fi";
  }
  if (!have) {
    if (pass != nullptr && pass[0] != '\0') {
      // A network with a password and still no key: a name or password
      // derive_from_wifi() cannot take, or an HMAC that failed. Said on every
      // join it happens on; the next join tries again.
      Serial.println("[PEER] No fleet key: none could be derived from this network and the hub has "
                     "handed none - the peer protocol stays OFF until the next join.");
    } else if (!s_warned_open) {
      s_warned_open = true;
      Serial.println("[PEER] No fleet key: this network has no password and the hub has handed none - "
                     "the peer protocol stays OFF rather than running unauthenticated.");
    }
    s_armed = false;
    memset(s_key, 0, sizeof(s_key));
    s_key_id[0] = '\0';
    return false;
  }
  const bool changed = !s_armed || memcmp(s_key, key, sizeof(key)) != 0;
  memcpy(s_key, key, sizeof(key));
  fleet_key::key_id_hex(s_key, s_key_id);
  s_armed = true;
  if (changed) {
    s_table = peer::Table{};   // a new key is a new fleet
    Serial.printf("[PEER] Fleet key %s from %s - heartbeats every %u s on %s:%u\n",
                  s_key_id, how, (unsigned)(peer::HEARTBEAT_MS / 1000u),
                  FLEET_BEACON_UDP_GROUP, (unsigned)FLEET_BEACON_UDP_PORT);
  }
  return true;
}

void disarm() {
  close_sock();
}

void tick(uint32_t now_ms, bool sta_online) {
  if (!s_begun || !s_armed) return;
  if (!sta_online) {
    close_sock();
    return;
  }
  const uint32_t ip = (uint32_t)WiFi.localIP();
  if (ip == 0) return;
  if (s_fd < 0 || ip != s_bound_ip) {
    if (!open_sock(ip)) return;
    if (!s_announced) {
      s_announced = true;
      Serial.println("[PEER] Mesh socket up (multicast heartbeat + unicast events, authenticated).");
    }
    s_next_hb_ms = now_ms;  // say hello at once on every (re)open
  }
  drain(now_ms);
  peer::expire(s_table, now_ms);
  if ((int32_t)(now_ms - s_next_hb_ms) >= 0) {
    send_heartbeat(now_ms);
    s_next_hb_ms = now_ms + peer::HEARTBEAT_MS;
  }
}

bool armed() { return s_armed; }
bool ready() { return s_fd >= 0; }
const peer::Table& table() { return s_table; }
int peers() { return peer::count(s_table); }

peer::Election election(uint32_t now_ms) {
  const bool hub = s_hooks.hub_ok && s_hooks.hub_ok();
  return peer::elect(s_table, s_id.fp, now_ms / 1000u, hub, now_ms);
}

size_t coord_word(char* out, size_t cap, uint32_t now_ms) {
  if (!s_armed) { if (out && cap) out[0] = '\0'; return 0; }
  return peer::coord_word(election(now_ms), out, cap);
}

bool send_event(const peer::Event& e, uint32_t now_ms) {
  if (!s_armed || s_fd < 0) return false;
  peer::Frame f;
  peer::put_event(f, e);
  stamp(f, now_ms);
  uint8_t buf[peer::MAX_FRAME];
  const size_t n = peer::encode(f, s_key, buf, sizeof(buf));
  if (!n) return false;
  // Unicast to a coordinating peer when there is one; the group otherwise.
  const peer::Election el = election(now_ms);
  uint32_t dst = 0;
  if (el.who == peer::Election::Who::Peer) {
    const int idx = peer::find(s_table, el.fp);
    if (idx >= 0) dst = s_table.peers[idx].ip;
  }
  return send_to(buf, n, dst);
}

const char* key_id() { return s_armed ? s_key_id : ""; }
uint32_t rx_frames() { return s_rx; }
uint32_t tx_frames() { return s_tx; }
uint32_t rejected() { return s_rejected; }

}  // namespace peer_link
}  // namespace net
}  // namespace canary
