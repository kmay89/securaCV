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
// Who may vote is F234's open decision (today a vote needs no presence row):
// every voter here holds a row, except where the table is smaller than the
// three-vote floor, which these cases leave alone.
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#pragma GCC diagnostic ignored "-Wstringop-truncation"
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
  cc::session_id_from_pubkey(n.pub, n.sid);
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
                                int32_t ahead_s = 0) {
  cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, nonce_byte);
  h.timestamp = (uint32_t)((int64_t)h.timestamp + ahead_s);   // the sender's clock, ahead or behind
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)tpl;
  p.detail_slot = (uint8_t)cc::DETAIL_NONE;
  p.urgency = (uint8_t)cc::CHIRP_URG_CAUTION;
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

// `rows` nearby rows, a chirp, then votes from the rows one by one, a second
// apart: the chirp holds until the vote that makes its voters more than half
// of the rows (and at least three), and is suppressed at that vote. With
// `whole` the route view, the health log, the votes after it and the prune
// are checked too.
void suppressed_at_more_than_half(size_t rows, bool whole) {
  boot_enabled();
  const std::vector<Neighbor> nearby = fill_nearby(rows);
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
    const uint32_t t0 = boot_enabled();
    std::vector<Neighbor> nearby = fill_nearby(3);
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
    const uint32_t t0 = boot_enabled();
    std::vector<Neighbor> nearby = fill_nearby(7);
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
  // votes 100 s later, and drops them 130 s later.
  for (const uint32_t late : {100000u, 130000u}) {
    const uint32_t t0 = boot_enabled(0xFFFFFFFFu - 50000u);
    std::vector<Neighbor> nearby = fill_nearby(3);
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
// copy arrived, outside its window, and the chirp is still shown.
void test_a_chirp_heard_again_keeps_one_row_and_its_window() {
  boot_enabled();   // the filter was cleared at this boot
  const auto advance = [](uint32_t ms) {
    host_sim::now_ms += ms;
    host_sim::wall_now += ms / 1000;
  };
  std::vector<Neighbor> nearby = fill_nearby(3);
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

// ── One nonce, one chirp (sweep F270) ────────────────────────────────────

// The item's probe, the wall clock moving with uptime: a chirp heard 250 s
// after the filter was cleared is suppressed there (and, before F269, pruned
// at 281 s); the same frame heard again at 310 s, 60 s old, was stored anew
// with no votes, as the filter had been cleared whole at 300 s. Now it is
// refused, and so are a neighbor's relay of it and the frame again at the
// last second of its freshness window, while a new chirp from the same
// originator at that moment is stored (the receiver still takes witnesses).
void test_a_suppressed_chirp_is_not_stored_again() {
  boot_enabled();   // the filter was cleared at this boot
  const auto advance = [](uint32_t ms) {
    host_sim::now_ms += ms;
    host_sim::wall_now += ms / 1000;
  };
  std::vector<Neighbor> nearby = fill_nearby(3);
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
// here, at a frame heard at 329.999 s; the old filter, cleared whole past
// every 300 s, at one heard at 300.999 s.
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
    {"a_suppressed_chirp_stops_at_once", test_a_suppressed_chirp_stops_at_once},
    {"a_suppressed_chirp_is_not_stored_again", test_a_suppressed_chirp_is_not_stored_again},
    {"a_nonce_outlives_the_freshness_window", test_a_nonce_outlives_the_freshness_window},
    {"a_held_chirp_takes_no_second_row", test_a_held_chirp_takes_no_second_row},
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
