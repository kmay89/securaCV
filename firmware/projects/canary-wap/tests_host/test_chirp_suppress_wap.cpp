// Host test: community suppression (spec §2.5.6) against the REAL
// chirp_channel.cpp, compiled on the host over stubs/mesh_net (an ESP-NOW
// send log, an in-memory NVS, OpenSSL's Ed25519 and SHA-256), with real
// signed frames: presence beacons fill the nearby table, a neighbor's signed
// witness puts a chirp in the recent table, and signed suppress votes
// (CHIRP_MSG_SUPPRESS_VOTE) from distinct session keys vote it down.
//
// The rule (sweep F233): a chirp is suppressed once its distinct voters are
// more than half of the nearby rows and at least three, counting only votes
// heard within SUPPRESS_WINDOW_MS (120 s) of the chirp's arrival here.
// Before F233 handle_suppress_vote() kept at most MAX_CONFIRMERS_PER_CHIRP
// (8) voters while its threshold was the row count / 2 (at least 3): from 18
// rows on the threshold passed 8 and no chirp could be suppressed; below
// that it took at least half rounded down (7 rows suppressed at 3 votes, an
// 8-row table at 4, half), and SUPPRESS_WINDOW_MS was read nowhere, so a vote
// counted at any age.
//
// The expected thresholds here come from the spec's words (an independent
// oracle: `more_than_half()`), never from the firmware's own function, and the
// test names no symbol F233 added, so the same file compiles against the code
// before it: that is how each case below was shown to fail there.
//
// Who may vote (sweep F234): a vote counts only from a key whose nearby row
// is PRESENCE_REQUIRED_MS (ten minutes) old, the rule an ACK meets (spec
// §3.4 rule 4, F311). It counted from any key that signed it, row or not,
// so one radio made as many keys as the threshold asked and suppressed any
// chirp. The voters in every case below hold rows that old before the chirp
// arrives (stay_present()); the cases under "Who may vote" hold the rule
// itself. Whether a stronger cost is asked of a voter, and whether an
// emergency template can be voted down at all, stay open (F234, F232).
//
// What a suppressed chirp does (sweep F269): it leaves the recent table at
// the vote that suppresses it, so nothing reads it after that vote: GET
// /api/chirp/recent does not list it, a confirmation neither validates nor
// relays it, a local confirm or dismiss finds nothing and sends nothing, and
// a later vote counts nowhere. It stayed until update()'s next prune, up to
// 30 s, and in that time it was listed ("suppressed": true), confirmable,
// and validated and relayed by the confirmations that reached it.
//
// One nonce, one chirp (sweep F270): a witness whose nonce the nonce filter
// or a row of the recent table holds is refused, and the filter holds a
// nonce for longer than the frame that carried it stays fresh. It was one
// filter cleared whole every 5 minutes, as long as the freshness window, so
// the same frame heard just after a reset was stored again: a suppressed
// chirp came back with no votes, and a held one took a second row. The
// freshness window the cases below hold the filter to is spec §3.3's as the
// receiver reads it: a timestamp at most 30 s ahead of its wall clock and at
// most 300 s behind it, in whole seconds.
//
// An ACK takes no chirp off this device (sweep F305): a signed RESOLVED ACK
// from one key with a nearby row set the chirp's `dismissed`, and the next
// prune dropped it, where suppression asks more than half the rows and at
// least three. A received ACK of any type but CONFIRMED now changes nothing,
// and a row the owner dismissed takes no confirmation before its prune.
// The cases that confirm or vote hold their keys present for ten minutes
// first (stay_present(); spec §3.4 rule 4, sweeps F311 and F234).
//
// Host-tested only: the stubs stand in for the radio and the clock; the
// Arduino compile is CI's.
//
// Run: ./test_chirp_suppress_wap [name]

#ifndef CHIRP_CHANNEL_CPP
#error "CHIRP_CHANNEL_CPP (absolute path to chirp_channel.cpp) must be defined"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The wall clock chirp_channel.cpp reads: synced (past MIN_UNIX_TIME), UTC,
// daytime, so a witness is fresh and night mode is off.
namespace host_sim {
inline time_t wall_now = 1760000000;   // 2025-10-09 08:53:20 UTC
inline struct tm wall_tm;
}  // namespace host_sim
inline time_t host_sim_time(time_t* out) {
  if (out != nullptr) *out = host_sim::wall_now;
  return host_sim::wall_now;
}
inline struct tm* host_sim_localtime(const time_t* t) {
  return gmtime_r(t, &host_sim::wall_tm);
}

// nvs_store.h's two chirp-namespace calls, over the stub NVS.
#define SECURACV_NVS_STORE_H
inline bool nvs_get_u8(const char* key, uint8_t* out_val) {
  Preferences p;
  p.begin("chirp", true);
  if (!p.isKey(key)) return false;
  *out_val = p.getUChar(key, 0);
  return true;
}
inline bool nvs_set_u8(const char* key, uint8_t val) {
  Preferences p;
  p.begin("chirp", false);
  return p.putUChar(key, val) == 1;
}

#define time(p) host_sim_time(p)
#define localtime(p) host_sim_localtime(p)
// The firmware file is held to the device build's warnings (see
// test_chirp_commands_wap.cpp); this test's own code keeps all of them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(__GNUC__) && !defined(__clang__)   // a GCC-only warning name
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include CHIRP_CHANNEL_CPP
#pragma GCC diagnostic pop
#undef time
#undef localtime

// The sketch provides this on a device (canary_wap.ino).
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }

namespace chirp_suppress {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)
// The same, naming the table size a failure happened at, or (CHECK_SEC) the
// second of uptime a frame was heard at.
#define CHECK_AT(rows, c)                                                            \
  do {                                                                               \
    ++g_checks;                                                                      \
    if (!(c)) {                                                                      \
      std::fprintf(stderr, "FAIL %s:%d (%zu nearby rows): %s\n", __FILE__, __LINE__, \
                   (size_t)(rows), #c);                                              \
      std::exit(1);                                                                  \
    }                                                                                \
  } while (0)

#define CHECK_SEC(sec, c)                                                                \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(c)) {                                                                          \
      std::fprintf(stderr, "FAIL %s:%d (heard %u s after boot, less 1 ms): %s\n", __FILE__, \
                   __LINE__, (unsigned)(sec), #c);                                       \
      std::exit(1);                                                                      \
    }                                                                                    \
  } while (0)

// The same, naming the filter scene: where in uptime the nonce went in, the
// traffic pattern, and how long after it went in the lookup was.
#define CHECK_HELD(off, pattern, after, c)                                                   \
  do {                                                                                       \
    ++g_checks;                                                                              \
    if (!(c)) {                                                                              \
      std::fprintf(stderr, "FAIL %s:%d (in at %u ms after boot, pattern %d, looked up %u ms " \
                   "later): %s\n", __FILE__, __LINE__, (unsigned)(off), (int)(pattern),       \
                   (unsigned)(after), #c);                                                   \
      std::exit(1);                                                                          \
    }                                                                                        \
  } while (0)

namespace cc = chirp_channel;

// ── The oracle: spec §2.5.6, in its own words ───────────────────────────

// "|suppressed_by| / nearby_count > 50% and |suppressed_by| >= 3": the
// fewest distinct voters that suppress a chirp with `rows` nearby rows.
size_t more_than_half(size_t rows) {
  size_t votes = 3;
  while (votes * 2 <= rows) ++votes;
  return votes;
}

// ── Frames ──────────────────────────────────────────────────────────────

// Another chirp device in range, with its own session key.
struct Neighbor {
  uint8_t priv[32];
  uint8_t pub[32];
  uint8_t sid[cc::SESSION_ID_SIZE];
  uint8_t mac[6];
};
Neighbor neighbor_of(uint8_t seed) {
  Neighbor n;
  memset(n.priv, seed, sizeof n.priv);
  n.priv[0] ^= 0xA5;
  n.priv[31] ^= 0x3C;
  Ed25519::derivePublicKey(n.pub, n.priv);
  CHECK(cc::session_id_from_pubkey(n.pub, n.sid));
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x01, seed};
  memcpy(n.mac, mac, sizeof mac);
  return n;
}
cc::ChirpHeader header_of(const Neighbor& n, cc::ChirpMsgType type, uint8_t nonce_byte) {
  cc::ChirpHeader h;
  memset(&h, 0, sizeof h);
  h.magic = cc::CHIRP_MAGIC;
  h.version = cc::PROTOCOL_VERSION;
  h.msg_type = type;
  memcpy(h.session_id, n.sid, sizeof h.session_id);
  h.timestamp = (uint32_t)host_sim::wall_now;
  memset(h.nonce, nonce_byte, sizeof h.nonce);
  h.nonce[0] = n.mac[5];
  return h;
}
template <typename P>
std::vector<uint8_t> frame_of(const cc::ChirpHeader& h, const P& p) {
  std::vector<uint8_t> f(sizeof h + sizeof p);
  memcpy(f.data(), &h, sizeof h);
  memcpy(f.data() + sizeof h, &p, sizeof p);
  return f;
}
// mesh_network::update() hands every ESP-NOW frame to the channel on the
// loop task.
void deliver(const Neighbor& n, const std::vector<uint8_t>& f) {
  cc::dispatch_espnow_message(n.mac, f.data(), (int)f.size(), -60);
}
std::vector<uint8_t> presence_of(const Neighbor& n) {
  cc::ChirpPresencePayload p;
  memset(&p, 0, sizeof p);
  p.listening = 1;
  p.last_chirp_age_min = 255;
  return frame_of(header_of(n, cc::CHIRP_MSG_PRESENCE, 0x70), p);
}
std::vector<uint8_t> witness_of(const Neighbor& n, uint8_t nonce_byte,
                                cc::ChirpTemplate tpl = cc::TPL_INFRA_POWER_OUT,
                                int32_t ahead_s = 0,
                                cc::ChirpUrgency urg = cc::CHIRP_URG_CAUTION) {
  cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, nonce_byte);
  h.timestamp = (uint32_t)((int64_t)h.timestamp + ahead_s);   // the sender's clock, ahead or behind
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)tpl;
  p.detail_slot = (uint8_t)cc::DETAIL_NONE;
  p.urgency = (uint8_t)urg;
  p.ttl_minutes = 15;
  memcpy(p.session_pubkey, n.pub, sizeof p.session_pubkey);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
// `relayer`'s relay of the witness frame `w` (hop 1), as relay_chirp()
// builds one: the same nonce and timestamp, the origin's key and signature in
// the signed_origin envelope, re-signed under the relayer's own key.
std::vector<uint8_t> relay_of(const Neighbor& relayer, const std::vector<uint8_t>& w) {
  cc::ChirpHeader h;
  cc::ChirpWitnessPayload p;
  memcpy(&h, w.data(), sizeof h);
  memcpy(&p, w.data() + sizeof h, sizeof p);
  memcpy(p.signed_origin_pubkey, p.session_pubkey, sizeof p.signed_origin_pubkey);
  memcpy(p.signed_origin_signature, p.signature, sizeof p.signed_origin_signature);
  memcpy(h.session_id, relayer.sid, sizeof h.session_id);
  h.hop_count = 1;
  memcpy(p.session_pubkey, relayer.pub, sizeof p.session_pubkey);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, relayer.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, relayer.priv, relayer.pub, canonical, cl);
  return frame_of(h, p);
}
// A signed confirmation (CHIRP_MSG_ACK, ack_type confirmed) of the chirp
// `nonce` names.
std::vector<uint8_t> confirm_of(const Neighbor& n, const uint8_t nonce[8]) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_ACK, 0x71);
  cc::ChirpAckPayload p;
  memset(&p, 0, sizeof p);
  memcpy(p.original_nonce, nonce, 8);
  p.ack_type = cc::CHIRP_ACK_CONFIRMED;
  memcpy(p.confirmer_session_pubkey, n.pub, sizeof p.confirmer_session_pubkey);
  uint8_t canonical[128];
  const size_t cl = cc::build_ack_canonical(nonce, cc::CHIRP_ACK_CONFIRMED, n.pub, canonical,
                                            sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
// A signed CHIRP_MSG_ACK of any ack_type (a byte on the wire, so every
// value, not only the three ChirpAckType names) for the chirp `nonce` names,
// signed as the spec's canonical says; `frame_byte` varies the header nonce.
std::vector<uint8_t> ack_of(const Neighbor& n, const uint8_t nonce[8], uint8_t ack_type,
                            uint8_t frame_byte = 0x73) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_ACK, frame_byte);
  cc::ChirpAckPayload p;
  memset(&p, 0, sizeof p);
  memcpy(p.original_nonce, nonce, 8);
  p.ack_type = ack_type;
  memcpy(p.confirmer_session_pubkey, n.pub, sizeof p.confirmer_session_pubkey);
  uint8_t canonical[128];
  const size_t cl = cc::build_ack_canonical(nonce, ack_type, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
// A signed suppress vote for the chirp `nonce` names. `frame_byte` varies
// the header's nonce, so a repeated vote is a new frame, not a replay of the
// same bytes (the vote's signature covers neither).
std::vector<uint8_t> vote_of(const Neighbor& n, const uint8_t nonce[8], uint8_t frame_byte = 0x72) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_SUPPRESS_VOTE, frame_byte);
  cc::ChirpSuppressVotePayload p;
  memset(&p, 0, sizeof p);
  memcpy(p.original_nonce, nonce, 8);
  memcpy(p.voter_session_pubkey, n.pub, sizeof p.voter_session_pubkey);
  uint8_t canonical[128];
  const size_t cl = cc::build_suppress_canonical(nonce, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}

// ── The receiver ────────────────────────────────────────────────────────

// A device powering up with its channel turned on (the owner's enable), at
// uptime `at_ms`, or an hour after the last scene when 0. update() prunes on
// a 30 s cadence kept in a function-local static that no boot resets, so the
// hour makes each scene's first pass prune, and no pass prunes again until
// the scene moves the clock 30 s on. Returns the scene's start.
uint32_t boot_enabled(uint32_t at_ms = 0) {
  host_sim::nvs->clear();
  *host_sim::espnow = host_sim::EspNow();
  host_sim::wall_now = 1760000000;
  host_sim::millis_step_ms = 0;
  host_sim::now_ms = at_ms != 0 ? at_ms : host_sim::now_ms + 3600000u;
  cc::g_state = cc::CHIRP_DISABLED;
  cc::g_initialized = false;
  cc::g_relay_enabled = true;
  cc::g_urgency_filter = cc::CHIRP_URG_INFO;
  memset(&cc::g_cooldown, 0, sizeof(cc::g_cooldown));
  cc::g_session_start_ms = 0;
  cc::g_last_presence_ms = 0;
  cc::g_last_chirp_sent_ms = 0;
  cc::g_muted = false;
  cc::g_mute_until_ms = 0;
  cc::g_status_view = decltype(cc::g_status_view)();
  cc::g_nearby_view = decltype(cc::g_nearby_view)();
  cc::g_recent_view = decltype(cc::g_recent_view)();
  cc::g_tables_changed = true;
  CHECK(cc::init());
  CHECK(cc::enable());
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);
  cc::update();   // the scene's first pass, and its prune
  return host_sim::now_ms;
}

// `rows` neighbors (seeds 1..rows) beacon; each holds a row.
std::vector<Neighbor> fill_nearby(size_t rows) {
  std::vector<Neighbor> out;
  for (size_t i = 0; i < rows; ++i) {
    out.push_back(neighbor_of((uint8_t)(i + 1)));
    deliver(out.back(), presence_of(out.back()));
  }
  CHECK_AT(rows, cc::g_nearby_count == rows);
  return out;
}

// The neighbors in `ns` keep beaconing, once a minute, for `minutes`
// (PRESENCE_REQUIRED_MS by default, so the rows their first beacons put in
// the nearby table are that old and a confirmation or a vote from any of
// them counts: spec §3.4 rule 4, sweeps F311 and F234). The wall clock moves
// with uptime and the loop task runs a pass each minute, as on a device. The
// cases call it before their witness arrives: a vote counts only within
// SUPPRESS_WINDOW_MS (two minutes) of the chirp's arrival.
void stay_present(const std::vector<Neighbor>& ns,
                  uint32_t minutes = cc::PRESENCE_REQUIRED_MS / 60000) {
  for (uint32_t m = 0; m < minutes; ++m) {
    host_sim::now_ms += 60000;
    host_sim::wall_now += 60;
    for (const Neighbor& n : ns) deliver(n, presence_of(n));
    cc::update();
  }
}

// The originator: a neighbor that holds no row, so the table is `rows`
// exactly. Its signed witness lands in the recent table; returns the slot.
const Neighbor kOriginator = neighbor_of(0xC8);
cc::ReceivedChirp* witness_arrives(uint8_t nonce_byte, uint8_t out_nonce[8]) {
  const size_t before = cc::g_recent_chirp_count;
  const std::vector<uint8_t> f = witness_of(kOriginator, nonce_byte);
  deliver(kOriginator, f);
  CHECK(cc::g_recent_chirp_count == before + 1);
  memcpy(out_nonce, f.data() + offsetof(cc::ChirpHeader, nonce), 8);
  for (size_t i = 0; i < cc::g_recent_chirp_count; ++i) {
    if (memcmp(cc::g_recent_chirps[i].nonce, out_nonce, 8) == 0) {
      CHECK(cc::g_recent_chirps[i].received_ms == host_sim::now_ms);
      CHECK(!cc::g_recent_chirps[i].suppressed);
      CHECK(cc::g_recent_chirps[i].suppress_count == 0);
      return &cc::g_recent_chirps[i];
    }
  }
  CHECK(false);
  return nullptr;
}

// Whether the recent table holds a row for the chirp `nonce` names, and how
// many.
size_t rows_holding(const uint8_t nonce[8]) {
  size_t n = 0;
  for (size_t i = 0; i < cc::g_recent_chirp_count; ++i) {
    if (memcmp(cc::g_recent_chirps[i].nonce, nonce, 8) == 0) ++n;
  }
  return n;
}
bool table_holds(const uint8_t nonce[8]) { return rows_holding(nonce) > 0; }
// Empties the recent table, as a dismiss and the prune would, so a case can
// hold the nonce filter alone to a frame.
void drop_all_rows() {
  memset(cc::g_recent_chirps, 0, cc::MAX_RECENT_CHIRPS * sizeof(cc::ReceivedChirp));
  cc::g_recent_chirp_count = 0;
}

// The frames this device has sent since `from` (the stub's send log), of
// `type`, and of those the relays: a witness at hop 1 or more.
size_t sent_of_type(size_t from, cc::ChirpMsgType type, bool relays_only = false) {
  size_t n = 0;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) {
    CHECK(sent[i].bytes.size() >= sizeof(cc::ChirpHeader));
    cc::ChirpHeader h;
    memcpy(&h, sent[i].bytes.data(), sizeof h);
    if (h.msg_type != type) continue;
    if (relays_only && h.hop_count == 0) continue;
    ++n;
  }
  return n;
}

size_t suppressed_lines() {
  return (size_t)std::count(g_health.begin(), g_health.end(),
                            std::string("chirp: community-suppressed"));
}

// What GET /api/chirp/recent shows for the chirp `nonce` names, once the
// loop task's pass has published it: -1 absent, 0 shown, 1 shown suppressed.
int recent_route_shows(const uint8_t nonce[8]) {
  cc::RecentTable t;
  memset(&t, 0xA5, sizeof t);
  cc::read_recent(&t);
  for (size_t i = 0; i < t.count; ++i) {
    if (memcmp(t.chirps[i].nonce, nonce, 8) == 0) return t.chirps[i].suppressed ? 1 : 0;
  }
  return -1;
}

// ── At each table size ──────────────────────────────────────────────────

// `rows` nearby rows ten minutes old, a chirp, then votes from the rows one
// by one, a second apart: the chirp holds until the vote that makes its
// voters more than half of the rows (and at least three), and is suppressed
// at that vote. With `whole` the route view, the health log, the votes after
// it and the prune are checked too.
void suppressed_at_more_than_half(size_t rows, bool whole) {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(rows);
  stay_present(nearby);   // the rows may vote (F234)
  uint8_t nonce[8];
  cc::ReceivedChirp* c = witness_arrives(0x40, nonce);
  const size_t needed = more_than_half(rows);
  CHECK_AT(rows, needed <= rows);   // reachable from the rows alone
  const size_t lines_before = suppressed_lines();

  for (size_t v = 0; v < needed; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce));
    if (v + 1 < needed) {
      CHECK_AT(rows, c->suppress_count == v + 1);     // every distinct vote counted
      CHECK_AT(rows, table_holds(nonce));             // half or fewer: held
      CHECK_AT(rows, suppressed_lines() == lines_before);
    }
  }
  // More than half: suppressed, and out of the table at that vote (F269).
  CHECK_AT(rows, !table_holds(nonce));
  CHECK_AT(rows, cc::g_recent_chirp_count == 0);
  CHECK_AT(rows, cc::g_nearby_count == rows);         // the table the votes were counted against
  if (!whole) return;

  CHECK_AT(rows, suppressed_lines() == lines_before + 1);   // logged at the vote that did it
  // The pass after the vote publishes it: the recent route no longer lists
  // it. It listed it, "suppressed": true, until the next prune (F269).
  cc::update();
  CHECK_AT(rows, recent_route_shows(nonce) == -1);
  cc::StatusView s;
  cc::read_status(&s);
  CHECK_AT(rows, s.nearby_count == rows);
  CHECK_AT(rows, s.recent_chirp_count == 0);

  // The set holds at least the most votes any table asks for (F233), so a
  // full table can reach its threshold.
  const size_t capacity = sizeof(c->suppressed_by) / sizeof(c->suppressed_by[0]);
  CHECK_AT(rows, capacity >= more_than_half(cc::MAX_NEARBY_CACHE));

  // The rest of the rows vote too, then devices that hold no row, more than
  // a set's worth: none of it counts anywhere, stores anything or logs the
  // line again (every vote after the one that suppressed logged it once
  // more before F233's review; until F269 they went on filling the set).
  // Since F234 a vote from a key with no row is refused before it looks the
  // chirp up, so the rows' own later votes are what reach the suppressed
  // chirp's empty slot (none at 3 rows, where all three suppressed it).
  for (size_t v = needed; v < rows; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce));
    CHECK_AT(rows, cc::g_recent_chirp_count == 0);
  }
  for (size_t k = 0; k < capacity + 3; ++k) {
    const Neighbor stranger = neighbor_of((uint8_t)(0x80 + k));
    host_sim::now_ms += 100;
    deliver(stranger, vote_of(stranger, nonce));
    CHECK_AT(rows, cc::g_recent_chirp_count == 0);
  }
  CHECK_AT(rows, suppressed_lines() == lines_before + 1);

  // The next prune finds nothing to drop, and the route stays as it was.
  host_sim::now_ms += 31000;
  cc::update();
  CHECK_AT(rows, recent_route_shows(nonce) == -1);
  CHECK_AT(rows, cc::g_recent_chirp_count == 0);
  CHECK_AT(rows, suppressed_lines() == lines_before + 1);
}

// The table sizes the item names: 3 (the floor), 7 and 8 (where at least
// half, rounded down, let a minority or a tie suppress), 16 and 17 (where
// the old threshold, 8, still fit the old set of 8), and 18, 20 and 32
// (where it passed the set and no chirp could be suppressed).
void test_the_named_table_sizes() {
  struct Size {
    size_t rows;
    size_t needed;   // spelled out, so the oracle is held too
  };
  const Size sizes[] = {{3, 3}, {7, 4}, {8, 5}, {16, 9}, {17, 9}, {18, 10}, {20, 11}, {32, 17}};
  for (const Size& s : sizes) {
    CHECK_AT(s.rows, more_than_half(s.rows) == s.needed);
    suppressed_at_more_than_half(s.rows, /*whole=*/true);
  }
}

// Every table size from the floor to a full table: suppression is reachable
// from the rows alone at each, at exactly more than half.
void test_every_table_size_can_suppress() {
  for (size_t rows = 3; rows <= cc::MAX_NEARBY_CACHE; ++rows) {
    suppressed_at_more_than_half(rows, /*whole=*/false);
  }
}

// ── A repeated voter ────────────────────────────────────────────────────

// A voter counts once, however often it votes and whichever frame carries
// it: at 7 rows (4 votes needed) three voters, each voting again, hold the
// chirp at three; the fourth distinct voter suppresses it. And a repeated
// vote never takes a slot: at 32 rows the 16 voters voting twice leave the
// count at 16, short of 17, and the 17th distinct voter suppresses.
void test_a_repeated_voter_counts_once() {
  boot_enabled();
  std::vector<Neighbor> nearby = fill_nearby(7);
  stay_present(nearby);
  uint8_t nonce[8];
  cc::ReceivedChirp* c = witness_arrives(0x41, nonce);
  for (size_t v = 0; v < 3; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x72));
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x73));   // the same voter, a new frame
    const std::vector<uint8_t> again = vote_of(nearby[v], nonce, 0x72);
    deliver(nearby[v], again);                              // and the same bytes again
    CHECK(c->suppress_count == v + 1);
    CHECK(table_holds(nonce));
  }
  host_sim::now_ms += 1000;
  deliver(nearby[0], vote_of(nearby[0], nonce, 0x74));      // the first voter, a fourth time
  CHECK(c->suppress_count == 3);
  CHECK(table_holds(nonce));
  deliver(nearby[3], vote_of(nearby[3], nonce));
  CHECK(!table_holds(nonce));                               // the fourth: suppressed, out

  boot_enabled();
  nearby = fill_nearby(32);
  stay_present(nearby);
  c = witness_arrives(0x42, nonce);
  for (size_t v = 0; v < 16; ++v) {
    host_sim::now_ms += 500;
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x72));
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x73));
  }
  CHECK(c->suppress_count == 16);
  CHECK(table_holds(nonce));
  deliver(nearby[16], vote_of(nearby[16], nonce));
  CHECK(!table_holds(nonce));                               // the 17th: suppressed, out
}

// ── Votes across the window ─────────────────────────────────────────────

// A vote counts within SUPPRESS_WINDOW_MS (120 s) of the chirp's arrival
// here, the last millisecond included, and is dropped after it, taking no
// slot. At 3 rows (3 votes needed, before F233 as now) two votes inside the
// window and a third at 120 001 ms leave the chirp shown; the same third
// vote at 120 000 ms suppresses it.
void test_votes_across_the_window() {
  for (const uint32_t third_at : {120000u, 120001u}) {
    boot_enabled();
    std::vector<Neighbor> nearby = fill_nearby(3);
    stay_present(nearby);
    const uint32_t t0 = host_sim::now_ms;
    uint8_t nonce[8];
    cc::ReceivedChirp* c = witness_arrives(0x43, nonce);
    CHECK(c->received_ms == t0);
    host_sim::now_ms = t0 + 1;
    deliver(nearby[0], vote_of(nearby[0], nonce));
    host_sim::now_ms = t0 + 60000;
    deliver(nearby[1], vote_of(nearby[1], nonce));
    CHECK(c->suppress_count == 2);
    host_sim::now_ms = t0 + third_at;
    deliver(nearby[2], vote_of(nearby[2], nonce));
    if (third_at <= 120000u) {
      CHECK(!table_holds(nonce));      // suppressed, out of the table
    } else {
      CHECK(c->suppress_count == 2);   // dropped, not stored for later
      CHECK(table_holds(nonce));
      // A voter inside the window still counts, and later votes still do not.
      host_sim::now_ms = t0 + 200000;
      deliver(nearby[2], vote_of(nearby[2], nonce, 0x75));
      CHECK(c->suppress_count == 2);
      CHECK(table_holds(nonce));
      cc::update();
      CHECK(recent_route_shows(nonce) == 0);   // still shown, not suppressed
    }
  }

  // The window is the chirp's own: a second chirp heard later opens its own
  // 120 s, while the first one's has closed. At 7 rows (4 votes needed).
  {
    boot_enabled();
    std::vector<Neighbor> nearby = fill_nearby(7);
    stay_present(nearby);
    const uint32_t t0 = host_sim::now_ms;
    uint8_t first[8];
    uint8_t second[8];
    cc::ReceivedChirp* a = witness_arrives(0x44, first);
    host_sim::now_ms = t0 + 100000;
    const size_t slot_b = cc::g_recent_chirp_count;
    witness_arrives(0x45, second);
    cc::ReceivedChirp* b = &cc::g_recent_chirps[slot_b];
    CHECK(memcmp(b->nonce, second, 8) == 0);
    a = &cc::g_recent_chirps[0];
    CHECK(memcmp(a->nonce, first, 8) == 0);
    host_sim::now_ms = t0 + 130000;   // 130 s after the first, 30 s after the second
    for (size_t v = 0; v < 4; ++v) {
      deliver(nearby[v], vote_of(nearby[v], first));
      deliver(nearby[v], vote_of(nearby[v], second));
    }
    CHECK(!table_holds(second));      // suppressed at its 4th vote, out of the table
    CHECK(table_holds(first));        // its window closed: no vote counted
    a = &cc::g_recent_chirps[0];
    CHECK(memcmp(a->nonce, first, 8) == 0);
    CHECK(a->suppress_count == 0 && !a->suppressed);
    (void)b;
  }

  // Uptime wraps (49.7 days): a chirp heard 50 s before the wrap still takes
  // votes 100 s later, and drops them 130 s later. The voters' rows were put
  // in ten minutes before the chirp, so their age is read across the wrap
  // too.
  for (const uint32_t late : {100000u, 130000u}) {
    boot_enabled(0xFFFFFFFFu - 50000u - cc::PRESENCE_REQUIRED_MS);
    std::vector<Neighbor> nearby = fill_nearby(3);
    stay_present(nearby);
    const uint32_t t0 = host_sim::now_ms;
    CHECK(t0 == 0xFFFFFFFFu - 50000u);
    uint8_t nonce[8];
    cc::ReceivedChirp* c = witness_arrives(0x46, nonce);
    host_sim::now_ms = t0 + late;   // past the wrap
    CHECK(host_sim::now_ms < t0);
    for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce));
    CHECK(table_holds(nonce) == (late > 120000u));   // suppressed (out) inside the window only
    if (late > 120000u) CHECK(c->suppress_count == 0);
  }
}

// One chirp, one row (sweep F270), and one window. The scene F233's review
// found: a witness heard 250 s after the filter was cleared, then the same
// frame again at 310 s, 60 s old. The filter was cleared whole every 300 s,
// so the frame passed it and took a second row, and votes at 380 s (130 s
// after the first copy, 70 s after the second) suppressed the second copy
// (this case was a_second_copy_takes_its_own_votes, which held that). Now
// the frame heard again takes no row, the votes fall 130 s after the only
// copy arrived, outside its window, and the chirp is still shown. The
// voters' rows are ten minutes old first (F234), so the window alone refuses
// their votes; the scene's times count from there.
void test_a_chirp_heard_again_keeps_one_row_and_its_window() {
  boot_enabled();
  const auto advance = [](uint32_t ms) {
    host_sim::now_ms += ms;
    host_sim::wall_now += ms / 1000;
  };
  std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);
  advance(250000);
  for (const Neighbor& n : nearby) deliver(n, presence_of(n));
  const std::vector<uint8_t> w = witness_of(kOriginator, 0x48);
  uint8_t nonce[8];
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(kOriginator, w);
  const uint32_t first_heard = host_sim::now_ms;
  CHECK(rows_holding(nonce) == 1);
  advance(60000);   // 310 s: past the old filter's reset; the frame is 60 s old
  deliver(kOriginator, w);
  CHECK(rows_holding(nonce) == 1);   // refused: no second row
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(cc::g_recent_chirps[0].received_ms == first_heard);

  advance(70000);   // 380 s
  for (const Neighbor& n : nearby) deliver(n, presence_of(n));
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce));
  CHECK(rows_holding(nonce) == 1);   // its window closed at 370 s: shown
  CHECK(cc::g_recent_chirps[0].suppress_count == 0);
  cc::update();
  CHECK(recent_route_shows(nonce) == 0);
}

// A vote counts only when it verifies: its signature over the chirp's nonce
// and the voter's key, by that key, from the session that key derives. At
// 3 rows (3 votes needed) four votes that fail one of those take no slot and
// log nothing; then the rows' own votes suppress the chirp at the third. With
// the signature check gone, anyone in range could vote from made-up keys;
// with the session-id check gone, a frame could speak for a session it is not.
void test_a_vote_must_verify() {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);   // each key may vote (F234): what refuses below is the frame
  uint8_t nonce[8];
  cc::ReceivedChirp* c = witness_arrives(0x49, nonce);
  const size_t lines_before = suppressed_lines();
  const size_t sig_at = sizeof(cc::ChirpHeader) + offsetof(cc::ChirpSuppressVotePayload, signature);
  const size_t target_at =
      sizeof(cc::ChirpHeader) + offsetof(cc::ChirpSuppressVotePayload, original_nonce);
  const size_t key_at =
      sizeof(cc::ChirpHeader) + offsetof(cc::ChirpSuppressVotePayload, voter_session_pubkey);

  // 1. A flipped bit in the signature.
  std::vector<uint8_t> flipped = vote_of(nearby[0], nonce, 0x60);
  flipped[sig_at + 17] ^= 0x04;
  // 2. A good signature under a header that names another row's session.
  std::vector<uint8_t> foreign_sid = vote_of(nearby[0], nonce, 0x61);
  memcpy(&foreign_sid[offsetof(cc::ChirpHeader, session_id)], nearby[1].sid, cc::SESSION_ID_SIZE);
  // 3. A vote signed for another chirp, its target rewritten to this one.
  uint8_t other[8];
  memcpy(other, nonce, 8);
  other[7] ^= 0xFF;
  std::vector<uint8_t> retargeted = vote_of(nearby[2], other, 0x62);
  memcpy(&retargeted[target_at], nonce, 8);
  // 4. A vote that names a key its sender does not hold: the first row signs
  // for the second's key, under the second's session id.
  std::vector<uint8_t> borrowed = vote_of(nearby[0], nonce, 0x63);
  {
    memcpy(&borrowed[key_at], nearby[1].pub, sizeof nearby[1].pub);
    memcpy(&borrowed[offsetof(cc::ChirpHeader, session_id)], nearby[1].sid, cc::SESSION_ID_SIZE);
    uint8_t canonical[128];
    const size_t cl = cc::build_suppress_canonical(nonce, nearby[1].pub, canonical, sizeof canonical);
    CHECK(cl > 0);
    Ed25519::sign(&borrowed[sig_at], nearby[0].priv, nearby[0].pub, canonical, cl);
  }
  const std::vector<uint8_t>* bad[] = {&flipped, &foreign_sid, &retargeted, &borrowed};
  for (const std::vector<uint8_t>* f : bad) {
    host_sim::now_ms += 1000;
    deliver(nearby[0], *f);
    CHECK(c->suppress_count == 0);
    CHECK(table_holds(nonce));
  }
  CHECK(suppressed_lines() == lines_before);

  // The rows' own votes, each signed as the firmware signs one, still count.
  for (size_t v = 0; v < 3; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce));
    if (v < 2) CHECK(c->suppress_count == v + 1);
  }
  CHECK(!table_holds(nonce));          // the third: suppressed, out of the table
  CHECK(suppressed_lines() == lines_before + 1);
}

// A vote heard before the chirp it names is not kept for it: the chirp
// arrives with no votes, and the voter must vote again.
void test_a_vote_before_its_chirp_is_not_kept() {
  boot_enabled();
  std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);
  const std::vector<uint8_t> w = witness_of(kOriginator, 0x47);
  uint8_t nonce[8];
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce));
  host_sim::now_ms += 1000;
  deliver(kOriginator, w);
  CHECK(cc::g_recent_chirp_count == 1);
  cc::ReceivedChirp* c = &cc::g_recent_chirps[0];
  CHECK(c->suppress_count == 0);
  CHECK(table_holds(nonce));
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce, 0x76));
  CHECK(!table_holds(nonce));          // suppressed, out of the table
}

// ── Who may vote (sweep F234) ───────────────────────────────────────────

// Every byte a vote could move: the recent table and the nearby table whole,
// their counts, and the health log's length.
struct Tables {
  std::vector<uint8_t> recent;
  size_t recent_count;
  std::vector<uint8_t> nearby;
  size_t nearby_count;
  size_t health_lines;
  bool operator==(const Tables& o) const {
    return recent == o.recent && recent_count == o.recent_count && nearby == o.nearby &&
           nearby_count == o.nearby_count && health_lines == o.health_lines;
  }
};
Tables tables_now() {
  Tables t;
  const uint8_t* r = reinterpret_cast<const uint8_t*>(cc::g_recent_chirps);
  const uint8_t* n = reinterpret_cast<const uint8_t*>(cc::g_nearby_devices);
  t.recent.assign(r, r + cc::MAX_RECENT_CHIRPS * sizeof(cc::ReceivedChirp));
  t.recent_count = cc::g_recent_chirp_count;
  t.nearby.assign(n, n + cc::MAX_NEARBY_CACHE * sizeof(cc::NearbyDevice));
  t.nearby_count = cc::g_nearby_count;
  t.health_lines = g_health.size();
  return t;
}
// Delivers `f` from `radio` and holds that it moved nothing: a refused vote.
// REFUSED() names the caller's line, and what moved.
void refused_at(int line, const Neighbor& radio, const std::vector<uint8_t>& f) {
  const Tables before = tables_now();
  deliver(radio, f);
  const Tables after = tables_now();
  ++g_checks;
  if (after == before) return;
  std::fprintf(stderr, "FAIL %s:%d: a refused vote moved %s%s%s%s(row 0's suppress_count %u -> %u)\n",
               __FILE__, line, after.recent != before.recent ? "the recent table " : "",
               after.recent_count != before.recent_count ? "the recent count " : "",
               after.nearby != before.nearby || after.nearby_count != before.nearby_count
                   ? "the nearby table " : "",
               after.health_lines != before.health_lines ? "the health log " : "",
               before.recent_count > 0 ? (unsigned)before.recent[offsetof(cc::ReceivedChirp, suppress_count)] : 0u,
               after.recent_count > 0 ? (unsigned)after.recent[offsetof(cc::ReceivedChirp, suppress_count)] : 0u);
  std::exit(1);
}
#define REFUSED(radio, f) refused_at(__LINE__, (radio), (f))

// An urgent emergency chirp from `origin` (one confirmation validates one,
// spec §3.4), stored here; its nonce in `nonce`.
cc::ReceivedChirp* emergency_arrives(const Neighbor& origin, uint8_t nonce_byte, uint8_t nonce[8]) {
  const std::vector<uint8_t> w =
      witness_of(origin, nonce_byte, cc::TPL_EMERG_FIRE_VISIBLE, 0, cc::CHIRP_URG_URGENT);
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(origin, w);
  for (size_t i = 0; i < cc::g_recent_chirp_count; ++i) {
    if (memcmp(cc::g_recent_chirps[i].nonce, nonce, 8) == 0) {
      CHECK(cc::g_recent_chirps[i].suppress_count == 0);
      return &cc::g_recent_chirps[i];
    }
  }
  CHECK(false);
  return nullptr;
}

// A key that no beacon has named here: a fresh session key, as one device
// makes any number of (spec §2.3).
Neighbor fresh_key(size_t k) { return neighbor_of((uint8_t)(0x80 + k)); }

// The item's probe (sweep F234): an urgent emergency chirp, at receivers
// whose rows are ten minutes old, voted on by fresh session keys, keys no
// beacon ever named here. They suppressed it at the 3rd vote with 0 or 2
// rows, the 4th with 6, the 9th with 17 and the 17th with 32: one radio in
// range hid any chirp. Now more than a full set of them (MAX_SUPPRESS_VOTERS
// + 3) each move no byte of either table and log nothing, and the chirp is
// still shown after the pass. Then the rows' own votes count, and suppress
// it at more than half of the rows (spec §2.5.6's arithmetic, F233, the same
// for the votes that count): at 0 and 2 rows nothing can, as three votes
// need three keys present here.
void test_a_fresh_key_votes_for_nothing() {
  for (const size_t rows : {0u, 2u, 6u, 17u, 32u}) {
    boot_enabled();
    const std::vector<Neighbor> nearby = fill_nearby(rows);
    stay_present(nearby);
    uint8_t nonce[8];
    cc::ReceivedChirp* c = emergency_arrives(kOriginator, 0x40, nonce);
    const size_t lines_before = suppressed_lines();
    const size_t capacity = sizeof(c->suppressed_by) / sizeof(c->suppressed_by[0]);
    for (size_t k = 0; k < capacity + 3; ++k) {
      host_sim::now_ms += 100;
      const Neighbor stranger = fresh_key(k);
      REFUSED(stranger, vote_of(stranger, nonce));
    }
    CHECK_AT(rows, c->suppress_count == 0 && table_holds(nonce));
    CHECK_AT(rows, cc::g_nearby_count == rows);   // a vote puts no row in
    cc::update();
    CHECK_AT(rows, recent_route_shows(nonce) == 0);

    const size_t needed = more_than_half(rows);
    for (size_t v = 0; v < rows && v < needed; ++v) {
      host_sim::now_ms += 100;
      deliver(nearby[v], vote_of(nearby[v], nonce));
      if (v + 1 < needed) CHECK_AT(rows, c->suppress_count == v + 1 && table_holds(nonce));
    }
    if (needed <= rows) {
      CHECK_AT(rows, !table_holds(nonce));                       // suppressed at `needed`
      CHECK_AT(rows, suppressed_lines() == lines_before + 1);
    } else {
      CHECK_AT(rows, c->suppress_count == rows && table_holds(nonce));   // every row voted: shown
      cc::update();
      CHECK_AT(rows, recent_route_shows(nonce) == 0);
      CHECK_AT(rows, suppressed_lines() == lines_before);
    }
  }
}

// A vote counts once its key's row is PRESENCE_REQUIRED_MS old, not before,
// the moment an ACK's would (spec §3.4 rule 4; test_chirp_security.cpp's
// c13_an_ack_waits_for_ten_minutes_of_presence). Three rows: b and c first
// heard a minute before a, all beaconing once a minute. An emergency chirp
// arrives when a's row is 9 minutes old; b's and c's votes count at 10
// minutes exactly, a's is refused at 9 minutes, at 9 minutes 59 s and at 10
// minutes less 1 ms, and none of those is kept for when its row comes of
// age: at 10 minutes the chirp still holds two votes. a's vote then counts,
// the third, and suppresses it.
void test_a_vote_waits_for_ten_minutes_of_presence() {
  boot_enabled();
  const Neighbor b = neighbor_of(0x01);
  const Neighbor c = neighbor_of(0x02);
  const Neighbor a = neighbor_of(0x03);
  deliver(b, presence_of(b));
  deliver(c, presence_of(c));
  stay_present({b, c}, 1);
  deliver(a, presence_of(a));
  const uint32_t a_first = host_sim::now_ms;
  stay_present({a, b, c}, 9);
  CHECK(host_sim::now_ms - a_first == 9u * 60000u);
  uint8_t nonce[8];
  cc::ReceivedChirp* ch = emergency_arrives(kOriginator, 0x41, nonce);
  deliver(b, vote_of(b, nonce));
  deliver(c, vote_of(c, nonce));
  CHECK(ch->suppress_count == 2 && table_holds(nonce));      // ten minutes exactly: counted

  uint8_t fb = 0x72;
  REFUSED(a, vote_of(a, nonce, fb++));                       // 9 minutes
  host_sim::now_ms = a_first + 9u * 60000u + 59000u;         // 9 minutes 59 s, a beacon just heard
  deliver(a, presence_of(a));
  REFUSED(a, vote_of(a, nonce, fb++));
  host_sim::now_ms = a_first + cc::PRESENCE_REQUIRED_MS - 1;
  REFUSED(a, vote_of(a, nonce, fb++));                       // 10 minutes less 1 ms
  host_sim::now_ms = a_first + cc::PRESENCE_REQUIRED_MS;
  cc::update();
  CHECK(ch->suppress_count == 2 && table_holds(nonce));      // nothing kept for this moment
  CHECK(recent_route_shows(nonce) == 0);
  const size_t lines_before = suppressed_lines();
  deliver(a, vote_of(a, nonce, fb++));                       // 10 minutes: the third vote
  CHECK(!table_holds(nonce));
  CHECK(suppressed_lines() == lines_before + 1);
}

// The ten minutes are the row's, as for an ACK (F311): a vote heard before
// its key's first beacon is refused, and so is the next one, once the beacon
// has put in a row that is new; a row pruned after NEARBY_TIMEOUT_MS of
// silence and put in again starts its ten minutes again, however long the
// key was present before. At 3 rows (3 votes needed), so the third key is
// the one that suppresses or not.
void test_a_new_row_starts_its_ten_minutes_again() {
  // Before its first beacon.
  {
    boot_enabled();
    const std::vector<Neighbor> bc = fill_nearby(2);
    stay_present(bc);
    const Neighbor k = neighbor_of(0x10);
    uint8_t nonce[8];
    cc::ReceivedChirp* ch = emergency_arrives(kOriginator, 0x42, nonce);
    REFUSED(k, vote_of(k, nonce, 0x72));                     // no row at all
    deliver(k, presence_of(k));                              // its first beacon: a row, new
    CHECK(cc::g_nearby_count == 3);
    REFUSED(k, vote_of(k, nonce, 0x73));
    deliver(bc[0], vote_of(bc[0], nonce));
    deliver(bc[1], vote_of(bc[1], nonce));
    CHECK(ch->suppress_count == 2 && table_holds(nonce));    // 3 rows ask 3 votes: shown
    cc::update();
    CHECK(recent_route_shows(nonce) == 0);
  }
  // Present ten minutes, silent until the prune takes its row, heard again.
  {
    boot_enabled();
    const std::vector<Neighbor> bc = fill_nearby(2);
    const Neighbor k = neighbor_of(0x10);
    deliver(k, presence_of(k));
    stay_present({bc[0], bc[1], k}, 20);                     // twenty minutes present
    for (int m = 0; m < 4 && cc::g_nearby_count == 3; ++m) stay_present(bc, 1);
    CHECK(cc::g_nearby_count == 2);                          // k's row pruned
    deliver(k, presence_of(k));                              // and put in again, new
    CHECK(cc::g_nearby_count == 3);
    uint8_t nonce[8];
    cc::ReceivedChirp* ch = emergency_arrives(kOriginator, 0x43, nonce);
    deliver(bc[0], vote_of(bc[0], nonce));
    deliver(bc[1], vote_of(bc[1], nonce));
    REFUSED(k, vote_of(k, nonce));
    CHECK(ch->suppress_count == 2 && table_holds(nonce));
    // Ten minutes after its return a vote of k's counts, for a chirp still in
    // its window.
    stay_present({bc[0], bc[1], k});
    uint8_t later[8];
    ch = emergency_arrives(kOriginator, 0x44, later);
    deliver(bc[0], vote_of(bc[0], later));
    deliver(bc[1], vote_of(bc[1], later));
    CHECK(ch->suppress_count == 2);
    deliver(k, vote_of(k, later));
    CHECK(!table_holds(later));                              // the third: suppressed
  }
}

// The rule reads the vote's key, never the radio it came from or its hop
// count: a vote carries no relay envelope, so a frame naming a key with no
// row here is refused from whichever radio, at hop 0 or 1, while a frame
// naming a present key counts whoever sends it. And the chirp's originator
// is a voter like any other (the rule is presence alone; spec §3.4 rule 2,
// no self-confirmation, is an ACK's): refused with no row, counted once its
// row is ten minutes old, the vote that suppresses at 3 rows.
void test_a_vote_is_ruled_by_its_key() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x20);
  std::vector<Neighbor> nearby = fill_nearby(2);
  stay_present(nearby);
  uint8_t nonce[8];
  cc::ReceivedChirp* ch = emergency_arrives(origin, 0x45, nonce);
  const Neighbor x = fresh_key(0);
  std::vector<uint8_t> relayed = vote_of(x, nonce);
  relayed[offsetof(cc::ChirpHeader, hop_count)] = 1;
  REFUSED(nearby[0], relayed);                               // x's vote from a present radio, hop 1
  REFUSED(x, vote_of(x, nonce, 0x73));                       // and from its own, hop 0
  REFUSED(origin, vote_of(origin, nonce));                   // the originator holds no row
  std::vector<uint8_t> carried = vote_of(nearby[0], nonce);
  carried[offsetof(cc::ChirpHeader, hop_count)] = 1;
  deliver(x, carried);                                       // a present key's vote, from x's radio
  CHECK(ch->suppress_count == 1);
  deliver(nearby[1], vote_of(nearby[1], nonce));
  CHECK(ch->suppress_count == 2 && table_holds(nonce));      // 2 rows ask 3: shown

  boot_enabled();
  nearby = fill_nearby(2);
  deliver(origin, presence_of(origin));
  stay_present({nearby[0], nearby[1], origin});
  ch = emergency_arrives(origin, 0x46, nonce);
  CHECK(cc::g_nearby_count == 3);
  const size_t before = host_sim::espnow->sent.size();
  deliver(origin, confirm_of(origin, nonce));                // its own ACK: refused, rule 2
  CHECK(ch->confirm_count == 0 && !ch->validated && sent_of_type(before, cc::CHIRP_MSG_WITNESS) == 0);
  deliver(nearby[0], vote_of(nearby[0], nonce));
  deliver(nearby[1], vote_of(nearby[1], nonce));
  CHECK(ch->suppress_count == 2);
  deliver(origin, vote_of(origin, nonce));                   // its own vote: the third
  CHECK(!table_holds(nonce));
}

// No other frame does a vote's work for a key with no row (F305 made every
// ACK but CONFIRMED change nothing; it holds here for keys F234 now
// refuses): a fresh key's RESOLVED ACK, every other ack_type byte, a MUTE
// and a SELFTEST frame take no vote, set nothing, and the chirp is still
// held and shown after the prune.
void test_no_other_frame_votes_for_a_fresh_key() {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);
  uint8_t nonce[8];
  emergency_arrives(kOriginator, 0x47, nonce);
  const Neighbor x = fresh_key(1);
  for (unsigned type = 0; type < 256; ++type) {
    if (type == cc::CHIRP_ACK_CONFIRMED) continue;
    deliver(x, ack_of(x, nonce, (uint8_t)type, (uint8_t)type));
  }
  cc::ChirpSuppressVotePayload body;   // a vote's own bytes, under the other two types
  memset(&body, 0, sizeof body);
  memcpy(body.original_nonce, nonce, 8);
  memcpy(body.voter_session_pubkey, x.pub, sizeof body.voter_session_pubkey);
  deliver(x, frame_of(header_of(x, cc::CHIRP_MSG_MUTE, 0x74), body));
  deliver(x, frame_of(header_of(x, cc::CHIRP_MSG_SELFTEST_OK, 0x75), body));
  host_sim::now_ms += 30001;                                 // the next prune
  cc::update();
  CHECK(rows_holding(nonce) == 1);
  const cc::ReceivedChirp* c = &cc::g_recent_chirps[0];
  CHECK(!c->dismissed && c->suppress_count == 0 && c->confirm_count == 0);
  CHECK(recent_route_shows(nonce) == 0);
}

// What the ten minutes cost (F234's floor is a delay, not a price; F232
// decides the price, open). Presence beacons are unsigned, so one radio
// beacons the session ids of keys it made, and their ten minutes run
// together. At a receiver with 3 present rows, one radio's 4 keys, beaconed
// from it ten minutes, make 7 rows, and their 4 votes suppress an emergency
// chirp (4 is more than half of 7). Pinned as today's behavior: a cost per
// row (F232) or a stronger rule for who may vote (F234) flips it, and this
// case is then rewritten to that rule, not deleted.
void test_keys_beaconed_ten_minutes_still_vote() {
  boot_enabled();
  std::vector<Neighbor> nearby = fill_nearby(3);
  std::vector<Neighbor> made;
  for (uint8_t k = 0; k < 4; ++k) {
    Neighbor n = neighbor_of((uint8_t)(0x60 + k));
    memcpy(n.mac, nearby[0].mac, sizeof n.mac);              // one radio, nearby[0]'s
    made.push_back(n);
    deliver(n, presence_of(n));
  }
  std::vector<Neighbor> all = nearby;
  all.insert(all.end(), made.begin(), made.end());
  stay_present(all);
  CHECK(cc::g_nearby_count == 7);
  uint8_t nonce[8];
  cc::ReceivedChirp* ch = emergency_arrives(kOriginator, 0x48, nonce);
  for (size_t v = 0; v < 3; ++v) {
    deliver(made[v], vote_of(made[v], nonce));
    CHECK(ch->suppress_count == v + 1);
  }
  deliver(made[3], vote_of(made[3], nonce));
  CHECK(!table_holds(nonce));                                // F234 and F232 open: suppressed
}

// ── A padded nearby table (sweep F232, open) ────────────────────────────

// A presence beacon claiming the made-up session id `sid`, from `radio`.
std::vector<uint8_t> presence_claiming(const Neighbor& radio, const uint8_t sid[cc::SESSION_ID_SIZE]) {
  cc::ChirpHeader h = header_of(radio, cc::CHIRP_MSG_PRESENCE, 0x70);
  memcpy(h.session_id, sid, cc::SESSION_ID_SIZE);
  cc::ChirpPresencePayload p;
  memset(&p, 0, sizeof p);
  p.listening = 1;
  p.last_chirp_age_min = 255;
  return frame_of(h, p);
}

// The item's scene: one radio beacons MAX_NEARBY_CACHE (32) made-up session
// ids, no key behind any of them, once a minute. A full table refuses a new
// row (handle_presence() evicts nothing), so a real neighbor r beaconing
// beside it for ten minutes never gets one: its CONFIRMED ACK of an
// emergency chirp is refused (no row; spec §3.4 rule 4), the chirp is not
// validated or relayed, and, since F234, its suppress vote is refused too.
// The made-up rows count toward the threshold all the same (17 votes of
// keys present here, at 32 rows), and no vote can come from them, as no key
// stands behind a made-up id; had the radio beaconed the ids of keys it
// made, those keys would vote after ten minutes
// (keys_beaconed_ten_minutes_still_vote). The control, without the padding:
// r's row comes of age, its ACK validates and relays the chirp, and its vote
// counts. Pinned as today's behavior (F232, open): a cost per presence row,
// a per-address cap or evicting the stalest row flips the padded half, r
// gets a row and both count after its ten minutes; rewrite it to that rule
// then, do not delete it.
void test_a_padded_table_keeps_a_neighbor_out() {
  for (const bool padded : {true, false}) {
    boot_enabled();
    const Neighbor padder = neighbor_of(0xB0);
    std::vector<std::array<uint8_t, cc::SESSION_ID_SIZE>> ids;
    if (padded) {
      for (size_t i = 0; i < cc::MAX_NEARBY_CACHE; ++i) {
        std::array<uint8_t, cc::SESSION_ID_SIZE> sid;
        for (size_t b = 0; b < sid.size(); ++b) sid[b] = (uint8_t)(0xE0 ^ (i * 37 + b * 11));
        ids.push_back(sid);
        deliver(padder, presence_claiming(padder, sid.data()));
      }
      CHECK(cc::g_nearby_count == cc::MAX_NEARBY_CACHE);
    }
    const Neighbor r = neighbor_of(0xB1);
    for (int m = 0; m < 10; ++m) {                           // ten minutes, both beaconing
      deliver(r, presence_of(r));
      for (const auto& sid : ids) deliver(padder, presence_claiming(padder, sid.data()));
      host_sim::now_ms += 60000;
      host_sim::wall_now += 60;
      cc::update();
    }
    deliver(r, presence_of(r));
    bool r_has_row = false;
    for (size_t i = 0; i < cc::g_nearby_count; ++i) {
      r_has_row = r_has_row || memcmp(cc::g_nearby_devices[i].session_id, r.sid, cc::SESSION_ID_SIZE) == 0;
    }
    CHECK(r_has_row == !padded);
    CHECK(cc::g_nearby_count == (padded ? cc::MAX_NEARBY_CACHE : 1u));

    uint8_t nonce[8];
    cc::ReceivedChirp* ch = emergency_arrives(kOriginator, 0x49, nonce);
    const size_t before = host_sim::espnow->sent.size();
    deliver(r, confirm_of(r, nonce));
    CHECK(ch->confirm_count == (padded ? 0u : 1u));
    CHECK(ch->validated == !padded && ch->relayed == !padded);
    CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS, /*relays_only=*/true) == (padded ? 0u : 1u));
    if (padded) {
      REFUSED(r, vote_of(r, nonce));                         // with F234: no row, no vote
    } else {
      deliver(r, vote_of(r, nonce));
      CHECK(ch->suppress_count == 1);
    }
    CHECK(table_holds(nonce));
    cc::update();
    CHECK(recent_route_shows(nonce) == 0);
  }
}

// ── A suppressed chirp stops at once (sweep F269) ────────────────────────

// The item's scene, at 3 rows (3 votes needed), for an infrastructure chirp
// (two confirmations validate one, spec §3.4) and an emergency one (one
// does). This device has met its presence requirement and its clock is set,
// so a local confirm could go out. First the control, two votes: the chirp
// is held, the confirmations validate it and relay it at hop 1, and a local
// confirm sends one ACK, so the harness sees a relay and a confirm when
// there is one. Then three votes: the chirp is out of the table at the third,
// the same confirmations validate nothing and relay nothing, the route never
// lists it, and a local confirm or dismiss finds nothing and sends nothing.
// Before F269 the third vote set `suppressed` and nothing on the way out read
// it until the next prune: the confirmations validated it and relayed it at
// hop 1, the route listed it ("suppressed": true), and a local confirm sent a
// signed ACK.
void test_a_suppressed_chirp_stops_at_once() {
  struct Kind {
    cc::ChirpTemplate tpl;
    size_t confirmations;   // that validate it: spec §3.4
  };
  const Kind kinds[] = {{cc::TPL_INFRA_POWER_OUT, 2}, {cc::TPL_EMERG_FIRE_VISIBLE, 1}};
  for (const Kind& k : kinds) {
    for (const size_t votes : {2u, 3u}) {
      boot_enabled();
      host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;   // this device may confirm
      const std::vector<Neighbor> nearby = fill_nearby(3);
      stay_present(nearby);                           // and so may they, and vote (F311, F234)
      const std::vector<uint8_t> w = witness_of(kOriginator, 0x50, k.tpl);
      uint8_t nonce[8];
      memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
      deliver(kOriginator, w);
      CHECK(rows_holding(nonce) == 1);
      for (size_t v = 0; v < votes; ++v) {
        host_sim::now_ms += 1000;
        deliver(nearby[v], vote_of(nearby[v], nonce));
      }
      const bool suppressed = votes == 3;
      CHECK(table_holds(nonce) == !suppressed);       // out at the third vote, before any pass

      // Neighbors confirm it: their signed ACKs, each from a row.
      const size_t before = host_sim::espnow->sent.size();
      for (size_t i = 0; i < k.confirmations; ++i) deliver(nearby[i], confirm_of(nearby[i], nonce));
      const size_t relays = sent_of_type(before, cc::CHIRP_MSG_WITNESS, /*relays_only=*/true);
      if (suppressed) {
        CHECK(relays == 0);                           // not relayed
        CHECK(cc::g_recent_chirp_count == 0);         // and nothing validated
      } else {
        CHECK(relays == 1);                           // the control: validated and relayed
        CHECK(cc::g_recent_chirps[0].validated && cc::g_recent_chirps[0].relayed);
        const cc::ChirpHeader* h =
            reinterpret_cast<const cc::ChirpHeader*>(host_sim::espnow->sent.back().bytes.data());
        CHECK(h->hop_count == 1 && memcmp(h->nonce, nonce, 8) == 0);
      }

      // The loop task's pass publishes what the routes show.
      cc::update();
      CHECK(recent_route_shows(nonce) == (suppressed ? -1 : 0));
      cc::StatusView sv;
      cc::read_status(&sv);
      CHECK(sv.recent_chirp_count == (suppressed ? 0 : 1));

      // The owner confirms it here, then dismisses it.
      const size_t mark = host_sim::espnow->sent.size();
      cc::ConfirmRefusal why = cc::CONFIRM_REFUSED_NONE;
      const bool confirmed = cc::confirm_chirp(nonce, &why);
      CHECK(confirmed == !suppressed);
      CHECK(why == (suppressed ? cc::CONFIRM_REFUSED_NOT_FOUND : cc::CONFIRM_REFUSED_NONE));
      CHECK(sent_of_type(mark, cc::CHIRP_MSG_ACK) == (suppressed ? 0u : 1u));
      bool vote_sent = false;
      cc::ConfirmRefusal vote_why = cc::CONFIRM_REFUSED_NONE;
      const bool dismissed = cc::dismiss_chirp(nonce, &vote_sent, &vote_why);
      CHECK(dismissed == !suppressed && vote_sent == !suppressed);
      CHECK(vote_why == (suppressed ? cc::CONFIRM_REFUSED_NOT_FOUND : cc::CONFIRM_REFUSED_NONE));
      CHECK(sent_of_type(mark, cc::CHIRP_MSG_SUPPRESS_VOTE) == (suppressed ? 0u : 1u));
      if (suppressed) CHECK(host_sim::espnow->sent.size() == mark);   // nothing at all
    }
  }
}

// The suppressed row is the one that leaves, wherever it sits: five chirps
// from five originators, held in arrival order, and the middle one
// suppressed at 3 rows. The other four stay, once each and in their order,
// the slot the table no longer uses is zeroed, the route lists the four, a
// confirmation still validates and relays the row that moved into the hole,
// and a new chirp is stored after them. The cases above suppress the only or
// the last row, where dropping the last row whatever was voted down, or
// moving the last row into the hole, looks the same as dropping the right one
// (F269's review: both passed every Chirp suite).
void test_a_suppressed_row_leaves_its_neighbors_in_order() {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);   // their confirmations and votes count below (F311, F234)
  std::vector<std::vector<uint8_t>> nonces;
  for (uint8_t i = 0; i < 5; ++i) {
    const Neighbor o = neighbor_of((uint8_t)(0xD0 + i));
    host_sim::now_ms += 1000;
    const std::vector<uint8_t> w = witness_of(o, (uint8_t)(0x30 + i));
    deliver(o, w);
    nonces.emplace_back(w.begin() + offsetof(cc::ChirpHeader, nonce),
                        w.begin() + offsetof(cc::ChirpHeader, nonce) + 8);
  }
  CHECK(cc::g_recent_chirp_count == 5);
  for (size_t i = 0; i < 5; ++i) CHECK(memcmp(cc::g_recent_chirps[i].nonce, nonces[i].data(), 8) == 0);

  host_sim::now_ms += 1000;
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonces[2].data()));
  CHECK(!table_holds(nonces[2].data()));
  CHECK(cc::g_recent_chirp_count == 4);
  const size_t kept[] = {0, 1, 3, 4};
  for (size_t j = 0; j < 4; ++j) {
    const uint8_t* n = nonces[kept[j]].data();
    CHECK(memcmp(cc::g_recent_chirps[j].nonce, n, 8) == 0);   // in arrival order
    CHECK(rows_holding(n) == 1);
    CHECK(cc::g_recent_chirps[j].suppress_count == 0);
  }
  static const uint8_t kZero[sizeof(cc::ReceivedChirp)] = {};
  CHECK(memcmp(&cc::g_recent_chirps[4], kZero, sizeof kZero) == 0);

  cc::update();
  cc::RecentTable t;
  memset(&t, 0xA5, sizeof t);
  cc::read_recent(&t);
  CHECK(t.count == 4);
  for (size_t j = 0; j < 4; ++j) CHECK(memcmp(t.chirps[j].nonce, nonces[kept[j]].data(), 8) == 0);

  // Two rows confirm the chirp now in the hole (infrastructure: two
  // validate it, spec §3.4): it is validated and relayed at hop 1.
  const size_t before = host_sim::espnow->sent.size();
  deliver(nearby[0], confirm_of(nearby[0], nonces[3].data()));
  deliver(nearby[1], confirm_of(nearby[1], nonces[3].data()));
  CHECK(cc::g_recent_chirps[2].validated && cc::g_recent_chirps[2].relayed);
  CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS, /*relays_only=*/true) == 1);
  const cc::ChirpHeader* h =
      reinterpret_cast<const cc::ChirpHeader*>(host_sim::espnow->sent.back().bytes.data());
  CHECK(h->hop_count == 1 && memcmp(h->nonce, nonces[3].data(), 8) == 0);
  for (size_t j : {0u, 1u, 3u}) CHECK(!cc::g_recent_chirps[j].validated);

  // A new chirp is stored after the four.
  const Neighbor late = neighbor_of(0xD5);
  host_sim::now_ms += 1000;
  const std::vector<uint8_t> w = witness_of(late, 0x35);
  deliver(late, w);
  CHECK(cc::g_recent_chirp_count == 5);
  CHECK(memcmp(cc::g_recent_chirps[4].nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8) == 0);
}

// ── An ACK takes no chirp off this device (sweep F305) ──────────────────

// The item's scene: 7 nearby rows (4 votes suppress a chirp there), an
// urgent emergency chirp, and one signed RESOLVED ACK from a nearby key that
// passes every check an ACK meets (a row ten minutes old, its session id the
// hash of its key, not the originator). handle_ack() set `dismissed` on it,
// and the next prune (update(), every 30 s) dropped the row: one neighbor
// took off this device a chirp that community suppression asks four of them
// to vote down. Now the chirp is held, listed and undismissed after the
// prune, with no suppress vote counted; and so after every other ack_type
// byte from every one of the seven keys. A CONFIRMED ACK from the same key
// then validates and relays it, so the keys were heard, and the type is what
// did nothing.
void test_a_resolved_ack_dismisses_nothing() {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(7);
  stay_present(nearby);
  const std::vector<uint8_t> w =
      witness_of(kOriginator, 0x54, cc::TPL_EMERG_FIRE_VISIBLE, 0, cc::CHIRP_URG_URGENT);
  uint8_t nonce[8];
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(kOriginator, w);
  CHECK(rows_holding(nonce) == 1);
  const size_t before = host_sim::espnow->sent.size();

  deliver(nearby[0], ack_of(nearby[0], nonce, cc::CHIRP_ACK_RESOLVED));
  host_sim::now_ms += 30001;                   // the next prune
  cc::update();
  CHECK(rows_holding(nonce) == 1);
  const cc::ReceivedChirp* c = &cc::g_recent_chirps[0];
  CHECK(!c->dismissed && !c->suppressed && c->suppress_count == 0);
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);
  CHECK(recent_route_shows(nonce) == 0);

  for (unsigned type = 0; type < 256; ++type) {
    if (type == cc::CHIRP_ACK_CONFIRMED) continue;
    for (const Neighbor& n : nearby) deliver(n, ack_of(n, nonce, (uint8_t)type, (uint8_t)type));
  }
  host_sim::now_ms += 30001;
  cc::update();
  CHECK(rows_holding(nonce) == 1);
  c = &cc::g_recent_chirps[0];
  CHECK(!c->dismissed && !c->suppressed && c->suppress_count == 0);
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);
  CHECK(recent_route_shows(nonce) == 0);
  CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS) == 0);   // nothing relayed

  deliver(nearby[0], confirm_of(nearby[0], nonce));
  CHECK(c->confirm_count == 1 && c->validated && c->relayed);
  CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS, /*relays_only=*/true) == 1);
}

// The owner's dismissal hides a chirp here at once, and the next prune takes
// its row, up to 30 s on. In that time a neighbor's confirmation found the
// row and validated and relayed it: a chirp the owner had just dismissed
// went out from this device (sweep F305, the item's second half). Now a
// dismissed row takes no confirmation; a chirp the owner did not dismiss,
// confirmed by the same keys at the same moment, is validated and relayed.
void test_a_dismissed_chirp_takes_no_confirmation() {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);                        // this device's ten minutes too: it may vote
  uint8_t dismissed[8], kept[8];
  const std::vector<uint8_t> wd =
      witness_of(kOriginator, 0x55, cc::TPL_EMERG_FIRE_VISIBLE, 0, cc::CHIRP_URG_URGENT);
  const Neighbor other = neighbor_of(0xC9);
  const std::vector<uint8_t> wk =
      witness_of(other, 0x56, cc::TPL_EMERG_FIRE_VISIBLE, 0, cc::CHIRP_URG_URGENT);
  memcpy(dismissed, wd.data() + offsetof(cc::ChirpHeader, nonce), 8);
  memcpy(kept, wk.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(kOriginator, wd);
  deliver(other, wk);
  CHECK(rows_holding(dismissed) == 1 && rows_holding(kept) == 1);

  bool vote_sent = false;
  cc::ConfirmRefusal why = cc::CONFIRM_REFUSED_NONE;
  CHECK(cc::dismiss_chirp(dismissed, &vote_sent, &why) && vote_sent);
  const size_t before = host_sim::espnow->sent.size();
  for (const Neighbor& n : nearby) deliver(n, confirm_of(n, dismissed));
  const cc::ReceivedChirp* d = &cc::g_recent_chirps[0];
  CHECK(memcmp(d->nonce, dismissed, 8) == 0 && d->dismissed);
  CHECK(d->confirm_count == 0 && !d->validated && !d->relayed);
  CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS) == 0);

  for (const Neighbor& n : nearby) deliver(n, confirm_of(n, kept));
  const cc::ReceivedChirp* k = &cc::g_recent_chirps[1];
  CHECK(memcmp(k->nonce, kept, 8) == 0 && k->validated && k->relayed);
  CHECK(sent_of_type(before, cc::CHIRP_MSG_WITNESS, /*relays_only=*/true) == 1);

  host_sim::now_ms += 30001;                   // the prune takes the dismissed row
  cc::update();
  CHECK(!table_holds(dismissed) && table_holds(kept));
}

// ── One nonce, one chirp (sweep F270) ────────────────────────────────────

// The item's probe, the wall clock moving with uptime: a chirp heard 250 s
// after the filter was cleared is suppressed there (and, before F269, pruned
// at 281 s); the same frame heard again at 310 s, 60 s old, was stored anew
// with no votes, as the filter had been cleared whole at 300 s. Now it is
// refused, and so are a neighbor's relay of it and the frame again at the
// last second of its freshness window, while a new chirp from the same
// originator at that moment is stored (the receiver still takes witnesses).
// The voters' rows are ten minutes old first (F234); the scene's times count
// from there.
void test_a_suppressed_chirp_is_not_stored_again() {
  boot_enabled();
  const auto advance = [](uint32_t ms) {
    host_sim::now_ms += ms;
    host_sim::wall_now += ms / 1000;
  };
  std::vector<Neighbor> nearby = fill_nearby(3);
  stay_present(nearby);
  advance(250000);
  for (const Neighbor& n : nearby) deliver(n, presence_of(n));
  const std::vector<uint8_t> w = witness_of(kOriginator, 0x51);
  const time_t sent_at = host_sim::wall_now;
  uint8_t nonce[8];
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(kOriginator, w);
  CHECK(rows_holding(nonce) == 1);
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce));
  CHECK(!table_holds(nonce));                         // suppressed
  advance(31000);                                     // 281 s: a prune
  cc::update();
  CHECK(cc::g_recent_chirp_count == 0);

  advance(29000);                                     // 310 s: the frame, 60 s old
  deliver(kOriginator, w);
  CHECK(cc::g_recent_chirp_count == 0);
  advance(10000);                                     // 320 s: a neighbor relays it
  deliver(nearby[0], relay_of(nearby[0], w));
  CHECK(cc::g_recent_chirp_count == 0);
  for (const Neighbor& n : nearby) deliver(n, presence_of(n));
  // The last second its timestamp is accepted: 300 s after it was sent.
  advance((uint32_t)(sent_at + 300 - host_sim::wall_now) * 1000);
  CHECK(host_sim::wall_now == sent_at + 300);
  for (uint32_t step = 0; step < 4; ++step) {          // 0, 333, 666 and 999 ms into that second
    deliver(kOriginator, w);
    deliver(nearby[1], relay_of(nearby[1], w));
    CHECK(cc::g_recent_chirp_count == 0);
    host_sim::now_ms += 333;
    cc::update();                                     // passes run; the filter turns if due
  }
  const std::vector<uint8_t> fresh = witness_of(kOriginator, 0x52);
  deliver(kOriginator, fresh);                        // the control: a new chirp is stored
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(memcmp(cc::g_recent_chirps[0].nonce, fresh.data() + offsetof(cc::ChirpHeader, nonce), 8) == 0);
}

// The filter holds every nonce for as long as the frame that carried it can
// still be accepted, whenever it was heard. The worst case is a frame sent
// with its timestamp 30 s ahead of this device's clock (the most the window
// takes) heard at the first millisecond of a wall-clock second: it stays
// acceptable to the last millisecond of the second 300 s past its timestamp,
// 330.999 s later. Here such a frame is heard 1 ms before each whole second
// of uptime from 1 to 700 s after boot, with the loop task's pass at every
// whole second and the wall clock's seconds starting 1 ms before the uptime
// clock's. Then the frame is heard again at the last millisecond the window
// takes it: refused, every time. A half of 330 s (no second's margin) fails
// here, at a frame heard at 329.999 s; the old filter, cleared whole at the
// first lookup 300 s or more after its last clearing, fails at every offset,
// from the first (heard 0.999 s after boot).
void test_a_nonce_outlives_the_freshness_window() {
  const time_t w0 = 1760000000;
  for (uint32_t k = 1; k <= 700; ++k) {
    const uint32_t t0 = boot_enabled();
    // The receiver's clocks, `ms` after boot.
    const auto at = [&](uint32_t ms) {
      host_sim::now_ms = t0 + ms;
      host_sim::wall_now = w0 + (time_t)((ms + 1) / 1000);
    };
    for (uint32_t sec = 1; sec < k; ++sec) {           // the passes before the frame
      at(sec * 1000);
      cc::update();
    }
    const uint32_t heard = k * 1000 - 1;
    at(heard);
    const std::vector<uint8_t> w = witness_of(kOriginator, (uint8_t)(0x60 + (k & 0x1F)),
                                              cc::TPL_INFRA_POWER_OUT, /*ahead_s=*/30);
    uint8_t nonce[8];
    memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
    deliver(kOriginator, w);
    CHECK_SEC(k, rows_holding(nonce) == 1);             // accepted: 30 s ahead is in the window
    const uint32_t ts = (uint32_t)host_sim::wall_now + 30;
    drop_all_rows();                                   // only the filter holds it now
    // The last millisecond of the second 300 s past its timestamp.
    const uint32_t last = (uint32_t)((ts + 300 - w0 + 1) * 1000 - 2);
    CHECK_SEC(k, last - heard == 330999);
    for (uint32_t sec = k; sec * 1000 <= last; ++sec) {
      at(sec * 1000);
      cc::update();
    }
    at(last);
    CHECK_SEC(k, (uint32_t)host_sim::wall_now == ts + 300);   // still in the window
    deliver(kOriginator, w);
    CHECK_SEC(k, cc::g_recent_chirp_count == 0);        // refused
  }
}

// A row of the recent table holds its nonce too, longer than the filter
// does (a row lives up to 30 min): with the wall clock not set, a witness
// has no freshness window, and the same frame heard 700 s later (past
// anything the filter holds) is refused while its chirp is still shown.
// Before F270 it took a second row once the filter had been cleared.
void test_a_held_chirp_takes_no_second_row() {
  boot_enabled();
  host_sim::wall_now = 1000;   // not set (before MIN_UNIX_TIME)
  const std::vector<uint8_t> w = witness_of(kOriginator, 0x53);
  uint8_t nonce[8];
  memcpy(nonce, w.data() + offsetof(cc::ChirpHeader, nonce), 8);
  deliver(kOriginator, w);
  CHECK(rows_holding(nonce) == 1);
  CHECK(cc::g_recent_chirps[0].unverifiable_timestamp);
  for (uint32_t s = 0; s < 700; s += 10) {
    host_sim::now_ms += 10000;
    cc::update();
    deliver(kOriginator, w);
    CHECK(rows_holding(nonce) == 1);
  }
  cc::update();
  CHECK(recent_route_shows(nonce) == 0);
}

// ── How long the filter holds a nonce (sweep F270, spec §5.3) ───────────

// An 8-byte nonce no frame in this file carries, numbered.
void filter_nonce(uint32_t id, uint8_t out[8]) {
  out[0] = 0xF1;
  out[1] = 0x7E;
  memcpy(out + 2, &id, sizeof id);
  out[6] = (uint8_t)(id * 7u);
  out[7] = 0x5A;
}

// The filter finds a nonce at every lookup up to NONCE_FILTER_HALF_MS
// (331 s) after it went in and at none from twice that (662 s) on, however
// sparse or busy the lookups between, and once it has forgotten a nonce it
// does not find it again. Each scene boots the receiver, puts one nonce in
// at an offset from boot across three halves (slot edges included, with no
// lookup before it, so an insert past a due turn must take that turn
// itself), then looks it up at each delay of one pattern, each lookup
// turning the halves as a witness's would. The patterns: one lookup at a
// half; one at two halves; one a half and a millisecond in, then two halves
// (the turn taken a whole half late); one a millisecond short of a half,
// then two halves; one a millisecond short of two halves, then two halves;
// one every 81 s to past two halves; and 1 ms, a half, a half and 1 ms, two
// halves less 1 ms and two halves. The turn was dated when it was taken, so
// the third pattern found a nonce put in at a half's start 662 s later, and
// up to 993 s with sparser frames (F270's review); a turn that cleared
// nothing, a lookup or an insert that took no turn, and a long gap that
// cleared only one half each fail here too.
void test_the_filter_holds_a_nonce_one_half_to_two() {
  const uint32_t H = cc::NONCE_FILTER_HALF_MS;
  const uint32_t offsets[] = {0,         1,     999,       H / 2,     H - 1,         H,
                              H + 1,     2 * H - 1, 2 * H, 2 * H + 1, 5 * H / 2,     3 * H - 1};
  std::vector<std::vector<uint32_t>> patterns = {
      {H},
      {2 * H},
      {H + 1, 2 * H},
      {H - 1, 2 * H},
      {2 * H - 1, 2 * H},
      {},
      {1, H, H + 1, 2 * H - 1, 2 * H},
  };
  for (uint32_t d = 81000; d <= 2 * H + 81000; d += 81000) patterns[5].push_back(d);
  uint32_t id = 0;
  for (const uint32_t off : offsets) {
    for (size_t p = 0; p < patterns.size(); ++p) {
      const uint32_t t0 = boot_enabled();
      uint8_t n[8];
      filter_nonce(++id, n);
      host_sim::now_ms = t0 + off;
      cc::cache_nonce(n);   // the frame's insert, the first filter call since boot
      bool forgotten = false;
      for (const uint32_t after : patterns[p]) {
        host_sim::now_ms = t0 + off + after;
        const bool found = cc::is_nonce_seen(n);
        if (after <= H) CHECK_HELD(off, p, after, found);              // held a half
        if (after >= 2 * H) CHECK_HELD(off, p, after, !found);         // gone at two
        if (forgotten) CHECK_HELD(off, p, after, !found);              // and stays gone
        forgotten = forgotten || !found;
      }
    }
  }
}

// The loop's pass turns the halves too, on or off, so a channel that hears
// no witness for 49.7 days still forgets on time across uptime's wrap: a
// nonce put in just after boot is gone at a lookup 2^32 ms and 20 s later,
// which, by itself, reads 20 s since the last turn it knows of. The passes
// run every 30 s here; the loop's run far more often.
void test_the_filter_turns_on_a_quiet_channel() {
  for (const bool on : {true, false}) {
    const uint32_t t0 = boot_enabled();
    if (!on) cc::disable();
    uint8_t n[8];
    filter_nonce(on ? 0xA11CEu : 0xB0B0u, n);
    cc::cache_nonce(n);
    uint64_t elapsed = 0;
    while (elapsed < (1ull << 32)) {
      elapsed += 30000;
      host_sim::now_ms = t0 + (uint32_t)elapsed;
      cc::update();
    }
    host_sim::now_ms = t0 + 20000;   // 2^32 ms + 20 s after it went in
    CHECK(cc::is_enabled() == on);
    CHECK(!cc::is_nonce_seen(n));
  }
}

// Under steady traffic the filter keeps forgetting: a witness's lookup and
// insert every 81 ms for five halves (4,096 nonces a half, the most spec
// §5.3 sizes for), the nonces random as a sender's are, then 2,000 nonces
// never heard. Spec §5.3 puts a half's false-positive rate at 4,096 nonces
// near 2.4 % and a lookup reads both halves, so up to about twice that: at
// most 4.8 % of the flood's own lookups after its first two halves may come
// back as repeats, and of the 2,000, looked up with both halves full (4.7 %
// expected), at most 6 %, room for the sample. A filter whose turns cleared
// nothing kept the whole flood and took about four new nonces in ten for
// repeats (F270's review).
void test_the_filter_forgets_under_steady_traffic() {
  const uint32_t t0 = boot_enabled();
  const uint32_t H = cc::NONCE_FILTER_HALF_MS;
  std::mt19937 rng(270);
  const auto random_nonce = [&rng](uint8_t out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = (uint8_t)rng();
  };
  size_t late_lookups = 0;
  size_t late_repeats = 0;
  for (uint32_t at = 0; at < 5 * H; at += 81) {
    host_sim::now_ms = t0 + at;
    uint8_t n[8];
    random_nonce(n);
    const bool seen = cc::is_nonce_seen(n);
    if (at >= 2 * H) {
      ++late_lookups;
      late_repeats += seen ? 1 : 0;
    }
    cc::cache_nonce(n);
  }
  size_t repeats = 0;
  for (int i = 0; i < 2000; ++i) {
    uint8_t n[8];
    random_nonce(n);
    repeats += cc::is_nonce_seen(n) ? 1 : 0;
  }
  std::printf("  steady traffic: %zu of %zu flood lookups and %zu of 2000 new nonces read as repeats\n",
              late_repeats, late_lookups, repeats);
  CHECK(late_lookups > 12000);
  CHECK(late_repeats * 1000 <= late_lookups * 48);
  CHECK(repeats <= 120);
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"the_named_table_sizes", test_the_named_table_sizes},
    {"every_table_size_can_suppress", test_every_table_size_can_suppress},
    {"a_repeated_voter_counts_once", test_a_repeated_voter_counts_once},
    {"votes_across_the_window", test_votes_across_the_window},
    {"a_chirp_heard_again_keeps_one_row_and_its_window",
     test_a_chirp_heard_again_keeps_one_row_and_its_window},
    {"a_vote_must_verify", test_a_vote_must_verify},
    {"a_vote_before_its_chirp_is_not_kept", test_a_vote_before_its_chirp_is_not_kept},
    {"a_fresh_key_votes_for_nothing", test_a_fresh_key_votes_for_nothing},
    {"a_vote_waits_for_ten_minutes_of_presence", test_a_vote_waits_for_ten_minutes_of_presence},
    {"a_new_row_starts_its_ten_minutes_again", test_a_new_row_starts_its_ten_minutes_again},
    {"a_vote_is_ruled_by_its_key", test_a_vote_is_ruled_by_its_key},
    {"no_other_frame_votes_for_a_fresh_key", test_no_other_frame_votes_for_a_fresh_key},
    {"keys_beaconed_ten_minutes_still_vote", test_keys_beaconed_ten_minutes_still_vote},
    {"a_padded_table_keeps_a_neighbor_out", test_a_padded_table_keeps_a_neighbor_out},
    {"a_suppressed_chirp_stops_at_once", test_a_suppressed_chirp_stops_at_once},
    {"a_suppressed_chirp_is_not_stored_again", test_a_suppressed_chirp_is_not_stored_again},
    {"a_nonce_outlives_the_freshness_window", test_a_nonce_outlives_the_freshness_window},
    {"a_held_chirp_takes_no_second_row", test_a_held_chirp_takes_no_second_row},
    {"a_suppressed_row_leaves_its_neighbors_in_order",
     test_a_suppressed_row_leaves_its_neighbors_in_order},
    {"a_resolved_ack_dismisses_nothing", test_a_resolved_ack_dismisses_nothing},
    {"a_dismissed_chirp_takes_no_confirmation", test_a_dismissed_chirp_takes_no_confirmation},
    {"the_filter_holds_a_nonce_one_half_to_two", test_the_filter_holds_a_nonce_one_half_to_two},
    {"the_filter_turns_on_a_quiet_channel", test_the_filter_turns_on_a_quiet_channel},
    {"the_filter_forgets_under_steady_traffic", test_the_filter_forgets_under_steady_traffic},
};

}  // namespace chirp_suppress

int main(int argc, char** argv) {
  using namespace chirp_suppress;
  const char* only = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const Test& t : kTests) {
    if (only != nullptr && std::strstr(t.name, only) == nullptr) continue;
    t.fn();
    std::printf("PASS %s\n", t.name);
    ++ran;
  }
  CHECK(ran > 0);
  if (only != nullptr) {
    std::printf("%d of %zu tests run (%d checks), filter \"%s\"\n", ran,
                sizeof kTests / sizeof kTests[0], g_checks, only);
    return 0;
  }
  std::printf("ALL %d chirp suppress checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
