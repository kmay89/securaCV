/**
 * @file peer_protocol.h
 * @brief The hub-less fleet's one peer protocol — authenticated heartbeats
 *        and events on the LAN, a peer table, and the coordinator rule. Pure.
 *
 * What exists today is the fleet-link presence beacon: 11/13 unsigned bytes
 * on BLE, ESP-NOW and UDP multicast 239.83.67.86:5867 (fleet_beacon*.h),
 * received by displays only. The program brief asks for a mesh every node
 * speaks: a multicast heartbeat, unicast events in one schema, a
 * deterministic coordinator that yields when a hub appears, and acceptance
 * only between paired nodes. This header is that protocol's bytes and rules;
 * the I/O (one lwIP socket on the SAME group and port the beacon already
 * uses, so no new firewall story) lives in peer_link.cpp beside it.
 *
 * FRAME (little-endian, HEADER_BYTES + payload + TAG_BYTES):
 *
 *   [0..3]   "SCVP"           magic — distinct from the beacon's 0xFF 0xFF 0x10
 *   [4]      version          1
 *   [5]      type             1 heartbeat, 2 event, 3 claim (coordinator)
 *   [6]      flags            bit0 hub_ok (the sender is on its hub)
 *   [7]      role             1 witness, 2 display, 3 hub
 *   [8..15]  fp               the sender's pubkey fingerprint, 8 bytes — the
 *                             same bytes the chirp / beacon / "SCV-XXXX" name
 *                             derive from; the peer table's key
 *   [16..19] uptime_s         seconds since the sender booted
 *   [20..23] seq              per-boot counter, replay window
 *   [24..31] coord_fp         who the sender believes coordinates (zeros = hub
 *                             or unknown; see flags)
 *   [32]     payload_len      0..MAX_PAYLOAD
 *   [33]     reserved         0
 *   [34..]   payload
 *   [..+16]  tag              HMAC-SHA256(fleet key, everything above)[0..16]
 *
 * PAYLOADS (vocabulary only — AGENTS.md Invariants II/III: no identity, no
 * time finer than the ten-minute bucket, the ObjectClass words and nothing a
 * stranger could learn from):
 *
 *   heartbeat (8 bytes): onboarding(1, onboard::State) hub(1, NS_HUB_*)
 *                        ota(1, OtaWord) rssi(int8) caps(uint16 LE)
 *                        peers_seen(1) saved_networks(1)
 *   event     (4 bytes): kind(1: 1 detect-edge, 2 tamper, 3 alert-clear,
 *                        4 ota-state) class(1: beacon detect_class vocabulary
 *                        0 none 1 person 2 vehicle 3 animal 4 package)
 *                        score(1: 0..100, 0xFF unknown) bucket(1: ten-minute
 *                        bucket of the day, low 8 bits)
 *
 * REPLAY: per peer, a frame is accepted when seq > last_seq, or when the
 * sender's uptime went DOWN (it rebooted and its counter restarted). Anything
 * else is a replay. A peer unheard for STALE_MS leaves the table.
 *
 * COORDINATOR: oldest uptime wins, tie → lowest fingerprint; every node
 * computes it from the same table, so the result is deterministic without a
 * round of messages. The moment ANY node (self or a peer) reports hub_ok,
 * the coordinator is the hub — every node yields on the next evaluation,
 * which is the graceful hand-off; when the hub is gone from every heartbeat,
 * the rule elects again. "Coordinator" governs only what the hub otherwise
 * would: who answers the roll-call for the fleet (serves peers' rows in
 * /api/fleet) and who runs a staged OTA. It never grants trust.
 *
 * Pure: no sockets, no heap. firmware/tests_host/test_peer_protocol.cpp.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fleet_key.h"

namespace canary {
namespace net {
namespace peer {

static constexpr uint8_t VERSION = 1;
static constexpr size_t FP_BYTES = 8;
static constexpr size_t HEADER_BYTES = 34;
static constexpr size_t MAX_PAYLOAD = 64;
static constexpr size_t TAG_BYTES = fleet_key::TAG_BYTES;
static constexpr size_t MAX_FRAME = HEADER_BYTES + MAX_PAYLOAD + TAG_BYTES;
static constexpr size_t MIN_FRAME = HEADER_BYTES + TAG_BYTES;

static constexpr uint32_t HEARTBEAT_MS = 10000;   ///< every node, every 10 s
static constexpr uint32_t STALE_MS = 35000;       ///< three missed heartbeats
static constexpr size_t MAX_PEERS = 16;

enum class Type : uint8_t { Heartbeat = 1, Event = 2, Claim = 3 };
enum Role : uint8_t { ROLE_WITNESS = 1, ROLE_DISPLAY = 2, ROLE_HUB = 3 };
enum Flag : uint8_t { FLAG_HUB_OK = 1u << 0 };

static constexpr size_t HEARTBEAT_PAYLOAD = 8;
static constexpr size_t EVENT_PAYLOAD = 4;
enum EventKind : uint8_t { EV_DETECT_EDGE = 1, EV_TAMPER = 2, EV_ALERT_CLEAR = 3, EV_OTA_STATE = 4 };

struct Frame {
  Type type = Type::Heartbeat;
  uint8_t flags = 0;
  uint8_t role = ROLE_WITNESS;
  uint8_t fp[FP_BYTES] = {0};
  uint32_t uptime_s = 0;
  uint32_t seq = 0;
  uint8_t coord_fp[FP_BYTES] = {0};
  uint8_t payload_len = 0;
  uint8_t payload[MAX_PAYLOAD] = {0};
};

struct Heartbeat {
  uint8_t onboarding = 0;
  uint8_t hub = 0;
  uint8_t ota = 0;
  int8_t rssi = 0;
  uint16_t caps = 0;
  uint8_t peers_seen = 0;
  uint8_t saved_networks = 0;
};

struct Event {
  uint8_t kind = 0;
  uint8_t object_class = 0;
  uint8_t score = 0xFF;
  uint8_t bucket = 0;
};

namespace detail {
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int fp_cmp(const uint8_t a[FP_BYTES], const uint8_t b[FP_BYTES]) { return memcmp(a, b, FP_BYTES); }
inline bool fp_zero(const uint8_t a[FP_BYTES]) {
  for (size_t i = 0; i < FP_BYTES; i++) if (a[i]) return false;
  return true;
}
}  // namespace detail

inline void put_heartbeat(Frame& f, const Heartbeat& h) {
  f.type = Type::Heartbeat;
  f.payload_len = HEARTBEAT_PAYLOAD;
  f.payload[0] = h.onboarding; f.payload[1] = h.hub; f.payload[2] = h.ota;
  f.payload[3] = (uint8_t)h.rssi;
  f.payload[4] = (uint8_t)h.caps; f.payload[5] = (uint8_t)(h.caps >> 8);
  f.payload[6] = h.peers_seen; f.payload[7] = h.saved_networks;
}
inline bool get_heartbeat(const Frame& f, Heartbeat& h) {
  if (f.type != Type::Heartbeat || f.payload_len < HEARTBEAT_PAYLOAD) return false;
  h.onboarding = f.payload[0]; h.hub = f.payload[1]; h.ota = f.payload[2];
  h.rssi = (int8_t)f.payload[3];
  h.caps = (uint16_t)f.payload[4] | ((uint16_t)f.payload[5] << 8);
  h.peers_seen = f.payload[6]; h.saved_networks = f.payload[7];
  return true;
}
inline void put_event(Frame& f, const Event& e) {
  f.type = Type::Event;
  f.payload_len = EVENT_PAYLOAD;
  f.payload[0] = e.kind; f.payload[1] = e.object_class; f.payload[2] = e.score; f.payload[3] = e.bucket;
}
inline bool get_event(const Frame& f, Event& e) {
  if (f.type != Type::Event || f.payload_len < EVENT_PAYLOAD) return false;
  e.kind = f.payload[0]; e.object_class = f.payload[1]; e.score = f.payload[2]; e.bucket = f.payload[3];
  return true;
}

/** @brief Serialize and tag. Returns bytes written, 0 on a cap too small or a
 *  tag the HMAC could not compute (the buffer is then wiped: no frame goes out
 *  under a zero tag). */
inline size_t encode(const Frame& f, const uint8_t key[fleet_key::KEY_BYTES], uint8_t* out, size_t cap) {
  if (out == nullptr || f.payload_len > MAX_PAYLOAD) return 0;
  const size_t n = HEADER_BYTES + f.payload_len + TAG_BYTES;
  if (cap < n) return 0;
  out[0] = 'S'; out[1] = 'C'; out[2] = 'V'; out[3] = 'P';
  out[4] = VERSION;
  out[5] = (uint8_t)f.type;
  out[6] = f.flags;
  out[7] = f.role;
  memcpy(out + 8, f.fp, FP_BYTES);
  detail::put32(out + 16, f.uptime_s);
  detail::put32(out + 20, f.seq);
  memcpy(out + 24, f.coord_fp, FP_BYTES);
  out[32] = f.payload_len;
  out[33] = 0;
  memcpy(out + HEADER_BYTES, f.payload, f.payload_len);
  if (!fleet_key::tag(key, out, HEADER_BYTES + f.payload_len, out + HEADER_BYTES + f.payload_len)) {
    memset(out, 0, n);
    return 0;
  }
  return n;
}

enum class Verdict : uint8_t { Ok = 0, TooShort, BadMagic, BadVersion, BadLength, BadTag, Replay, Self, Full };

inline const char* verdict_label(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::TooShort: return "too_short";
    case Verdict::BadMagic: return "bad_magic";
    case Verdict::BadVersion: return "bad_version";
    case Verdict::BadLength: return "bad_length";
    case Verdict::BadTag: return "bad_tag";
    case Verdict::Replay: return "replay";
    case Verdict::Self: return "self";
    case Verdict::Full: return "table_full";
  }
  return "unknown";
}

/** @brief Parse and verify the tag. The replay check is accept()'s. A tag the
 *  HMAC could not compute is BadTag, never a compare against zeros. */
inline Verdict decode(const uint8_t* in, size_t n, const uint8_t key[fleet_key::KEY_BYTES], Frame& out) {
  if (in == nullptr || n < MIN_FRAME) return Verdict::TooShort;
  if (in[0] != 'S' || in[1] != 'C' || in[2] != 'V' || in[3] != 'P') return Verdict::BadMagic;
  if (in[4] != VERSION) return Verdict::BadVersion;
  const uint8_t plen = in[32];
  if (plen > MAX_PAYLOAD || n != HEADER_BYTES + plen + TAG_BYTES) return Verdict::BadLength;
  uint8_t want[TAG_BYTES];
  if (!fleet_key::tag(key, in, HEADER_BYTES + plen, want) ||
      !fleet_key::tag_equal(want, in + HEADER_BYTES + plen)) {
    return Verdict::BadTag;
  }
  out.type = (Type)in[5];
  out.flags = in[6];
  out.role = in[7];
  memcpy(out.fp, in + 8, FP_BYTES);
  out.uptime_s = detail::get32(in + 16);
  out.seq = detail::get32(in + 20);
  memcpy(out.coord_fp, in + 24, FP_BYTES);
  out.payload_len = plen;
  memcpy(out.payload, in + HEADER_BYTES, plen);
  return Verdict::Ok;
}

// ── The peer table ────────────────────────────────────────────────────────
struct Peer {
  uint8_t fp[FP_BYTES] = {0};
  uint32_t last_seq = 0;
  uint32_t last_seen_ms = 0;
  uint32_t uptime_s = 0;      ///< at last_seen
  uint32_t ip = 0;            ///< the sender's address, as the socket saw it (0 = unknown)
  uint8_t role = 0;
  uint8_t flags = 0;
  uint8_t coord_fp[FP_BYTES] = {0};
  Heartbeat hb;
  bool used = false;
};

struct Table {
  Peer peers[MAX_PEERS] = {};
};

inline int find(const Table& t, const uint8_t fp[FP_BYTES]) {
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (t.peers[i].used && detail::fp_cmp(t.peers[i].fp, fp) == 0) return (int)i;
  }
  return -1;
}

inline int count(const Table& t) {
  int n = 0;
  for (size_t i = 0; i < MAX_PEERS; i++) n += t.peers[i].used ? 1 : 0;
  return n;
}

/**
 * @brief Take a verified frame into the table: replay check, slot, heartbeat
 * fields. `self_fp` frames are dropped (our own multicast echo).
 */
inline Verdict accept(Table& t, const Frame& f, uint32_t now_ms, uint32_t ip, const uint8_t self_fp[FP_BYTES]) {
  if (self_fp && detail::fp_cmp(f.fp, self_fp) == 0) return Verdict::Self;
  int idx = find(t, f.fp);
  if (idx >= 0) {
    Peer& p = t.peers[idx];
    const bool rebooted = f.uptime_s < p.uptime_s;
    if (!rebooted && f.seq <= p.last_seq) return Verdict::Replay;
  } else {
    for (size_t i = 0; i < MAX_PEERS; i++) {
      if (!t.peers[i].used) { idx = (int)i; break; }
    }
    if (idx < 0) return Verdict::Full;
    t.peers[idx] = Peer{};
    memcpy(t.peers[idx].fp, f.fp, FP_BYTES);
    t.peers[idx].used = true;
  }
  Peer& p = t.peers[idx];
  p.last_seq = f.seq;
  p.last_seen_ms = now_ms;
  p.uptime_s = f.uptime_s;
  p.ip = ip ? ip : p.ip;
  p.role = f.role;
  p.flags = f.flags;
  memcpy(p.coord_fp, f.coord_fp, FP_BYTES);
  if (f.type == Type::Heartbeat) get_heartbeat(f, p.hb);
  return Verdict::Ok;
}

/** @brief Drop peers unheard for `stale_ms`. Wrap-safe. */
inline void expire(Table& t, uint32_t now_ms, uint32_t stale_ms = STALE_MS) {
  for (size_t i = 0; i < MAX_PEERS; i++) {
    Peer& p = t.peers[i];
    if (p.used && (int32_t)(now_ms - p.last_seen_ms) > (int32_t)stale_ms) p.used = false;
  }
}

// ── The coordinator ───────────────────────────────────────────────────────
struct Election {
  enum class Who : uint8_t { Self = 0, Peer = 1, Hub = 2 } who = Who::Self;
  uint8_t fp[FP_BYTES] = {0};   ///< the coordinator's fingerprint (ours for Self; zeros for Hub)
};

/**
 * @brief Oldest uptime wins, tie → lowest fingerprint; a hub anywhere wins
 * outright. Peers' uptimes are aged to `now_ms` so a peer heard 30 s ago is
 * compared fairly against our live clock.
 */
inline Election elect(const Table& t, const uint8_t self_fp[FP_BYTES], uint32_t self_uptime_s,
                      bool self_hub_ok, uint32_t now_ms) {
  Election e;
  if (self_hub_ok) { e.who = Election::Who::Hub; return e; }
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (t.peers[i].used && (t.peers[i].flags & FLAG_HUB_OK)) { e.who = Election::Who::Hub; return e; }
  }
  e.who = Election::Who::Self;
  memcpy(e.fp, self_fp, FP_BYTES);
  uint32_t best_up = self_uptime_s;
  for (size_t i = 0; i < MAX_PEERS; i++) {
    const Peer& p = t.peers[i];
    if (!p.used) continue;
    const uint32_t age_s = (uint32_t)((int32_t)(now_ms - p.last_seen_ms) > 0 ? (now_ms - p.last_seen_ms) / 1000u : 0u);
    const uint32_t up = p.uptime_s + age_s;
    const bool older = up > best_up || (up == best_up && detail::fp_cmp(p.fp, e.fp) < 0);
    if (older) {
      best_up = up;
      e.who = Election::Who::Peer;
      memcpy(e.fp, p.fp, FP_BYTES);
    }
  }
  return e;
}

/// The `coord` TXT word / status word for an election.
inline size_t coord_word(const Election& e, char* out, size_t cap) {
  if (out == nullptr || cap < 17) return 0;
  if (e.who == Election::Who::Hub) { memcpy(out, "hub", 4); return 3; }
  if (e.who == Election::Who::Self) { memcpy(out, "self", 5); return 4; }
  static const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < FP_BYTES; i++) {
    out[2 * i] = hex[e.fp[i] >> 4];
    out[2 * i + 1] = hex[e.fp[i] & 0xf];
  }
  out[16] = '\0';
  return 16;
}

}  // namespace peer
}  // namespace net
}  // namespace canary
