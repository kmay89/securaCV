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
 *   [33]     boot_id          a random non-zero byte drawn once per boot
 *                             (esp_fill_random; 0 = a sender that predates it)
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
 * REPLAY: per peer, a frame is accepted when it advances the counter of the
 * boot the row tracks (boot_id equal, or either side without one: seq >
 * last_seq), or of the boot before it (a frame the receiver never saw; it
 * swaps the row back, since one device is in one boot at a time). Anything
 * else is a replay unless it claims a NEW boot (boot_id differs; without one,
 * uptime fell), and a claimed reboot is taken only from a boot's first
 * REBOOT_SEQ_WINDOW frames and first REBOOT_UPTIME_WINDOW_S seconds, at most
 * once per REBOOT_MIN_INTERVAL_MS per peer, and provisionally: it moves
 * presence only, until a second in-order frame of that boot confirms it. The
 * tag does not cover the source address, so the address is never taken from
 * a first sight or a provisional frame: only from a frame that advanced a
 * counter the row already held (the sender's unicast falls back to the group
 * until then). A genuine reboot the receiver hears late (past the window) is
 * refused until its row expires, STALE_MS later, and is then a new peer. A
 * peer unheard for STALE_MS leaves the table. docs/TRUST_MODEL.md §3 says
 * exactly what this holds and what it does not.
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

/// A claimed reboot is taken only from a boot's first frames (its counter
/// restarts at 1; a receiver that misses these waits out STALE_MS instead)...
static constexpr uint32_t REBOOT_SEQ_WINDOW = 8;
/// ...and its first seconds (a heartbeat every 10 s: the window above, plus
/// the Wi-Fi join)...
static constexpr uint32_t REBOOT_UPTIME_WINDOW_S = 120;
/// ...and at most once per peer in this interval (a board cannot reboot and
/// rejoin faster; a replayed first-boot frame buys one provisional sighting).
static constexpr uint32_t REBOOT_MIN_INTERVAL_MS = 30000;

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
  uint8_t boot_id = 0;        ///< random per boot, never 0 on a sender that has one
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
  out[33] = f.boot_id;
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
  out.boot_id = in[33];
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
  // The replay window (header REPLAY).
  uint8_t boot_id = 0;        ///< the boot the row tracks (0 = the sender has none)
  uint8_t prev_boot_id = 0;   ///< the boot before it, and where its counter was left
  uint32_t prev_last_seq = 0;
  bool pending = false;       ///< a claimed reboot seen once, not yet confirmed
  uint8_t pend_boot_id = 0;
  uint32_t pend_seq = 0;
  uint32_t pend_uptime_s = 0;
  bool reboot_seen = false;   ///< the rate limit on claimed reboots
  uint32_t reboot_ms = 0;
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

namespace detail {
/// The fields every accepted frame carries into its row.
inline void take_fields(Peer& p, const Frame& f, uint32_t now_ms) {
  p.last_seen_ms = now_ms;
  p.role = f.role;
  p.flags = f.flags;
  memcpy(p.coord_fp, f.coord_fp, FP_BYTES);
  if (f.type == Type::Heartbeat) get_heartbeat(f, p.hb);
}
/// A frame that advanced a counter the row held: the counter, the uptime
/// and (the one place it is taken from) the address.
inline void take_advance(Peer& p, const Frame& f, uint32_t now_ms, uint32_t ip) {
  p.last_seq = f.seq;
  p.uptime_s = f.uptime_s;
  p.ip = ip ? ip : p.ip;
  p.pending = false;
  take_fields(p, f, now_ms);
}
}  // namespace detail

/**
 * @brief Take a verified frame into the table: replay check, slot, heartbeat
 * fields. `self_fp` frames are dropped (our own multicast echo). The rules
 * are the header's REPLAY paragraph; `ip` is the socket's source address,
 * which the tag does not cover.
 */
inline Verdict accept(Table& t, const Frame& f, uint32_t now_ms, uint32_t ip, const uint8_t self_fp[FP_BYTES]) {
  if (self_fp && detail::fp_cmp(f.fp, self_fp) == 0) return Verdict::Self;
  int idx = find(t, f.fp);
  if (idx < 0) {
    for (size_t i = 0; i < MAX_PEERS; i++) {
      if (!t.peers[i].used) { idx = (int)i; break; }
    }
    if (idx < 0) return Verdict::Full;
    // First sight: the counters are taken at face value (there is nothing to
    // compare against), the address is not — the next in-order frame names it.
    Peer& p = t.peers[idx];
    p = Peer{};
    memcpy(p.fp, f.fp, FP_BYTES);
    p.used = true;
    p.boot_id = f.boot_id;
    p.last_seq = f.seq;
    p.uptime_s = f.uptime_s;
    detail::take_fields(p, f, now_ms);
    return Verdict::Ok;
  }
  Peer& p = t.peers[idx];
  const bool keyed = f.boot_id != 0 && p.boot_id != 0;
  // In order, in the boot the row tracks.
  if ((!keyed || f.boot_id == p.boot_id) && f.seq > p.last_seq) {
    detail::take_advance(p, f, now_ms, ip);
    return Verdict::Ok;
  }
  // The boot before, past where its counter was left: a frame this receiver
  // never saw, from the boot the sender is still in — the boot the row took
  // over it was a replay (or arrived out of order). Swap back.
  if (keyed && p.prev_boot_id != 0 && f.boot_id == p.prev_boot_id && f.seq > p.prev_last_seq) {
    const uint8_t was_boot = p.boot_id;
    const uint32_t was_seq = p.last_seq;
    p.boot_id = f.boot_id;
    p.prev_boot_id = was_boot;
    p.prev_last_seq = was_seq;
    detail::take_advance(p, f, now_ms, ip);
    return Verdict::Ok;
  }
  // Anything else is a replay unless it claims a NEW boot: one the row does
  // not know (a seen frame of either boot it knows is never a reboot).
  const bool claimed = keyed ? (f.boot_id != p.boot_id && f.boot_id != p.prev_boot_id)
                             : (f.uptime_s < p.uptime_s);
  if (!claimed) return Verdict::Replay;
  // The second in-order frame of a reboot seen once: confirmed. The boot the
  // row tracked becomes the one before, so a late frame of it is still known.
  if (p.pending && f.boot_id == p.pend_boot_id && f.seq > p.pend_seq && f.uptime_s >= p.pend_uptime_s) {
    p.prev_boot_id = p.boot_id;
    p.prev_last_seq = p.last_seq;
    p.boot_id = f.boot_id;
    detail::take_advance(p, f, now_ms, ip);
    return Verdict::Ok;
  }
  // First sight of a claimed reboot: inside the window, rate-limited, and
  // provisional — presence only, no counter, no uptime, no address.
  if (f.seq > REBOOT_SEQ_WINDOW || f.uptime_s >= REBOOT_UPTIME_WINDOW_S) return Verdict::Replay;
  if (p.reboot_seen && (uint32_t)(now_ms - p.reboot_ms) < REBOOT_MIN_INTERVAL_MS) return Verdict::Replay;
  p.pending = true;
  p.pend_boot_id = f.boot_id;
  p.pend_seq = f.seq;
  p.pend_uptime_s = f.uptime_s;
  p.reboot_seen = true;
  p.reboot_ms = now_ms;
  p.last_seen_ms = now_ms;
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
