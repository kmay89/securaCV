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
// The same, naming the table size a failure happened at.
#define CHECK_AT(rows, c)                                                            \
  do {                                                                               \
    ++g_checks;                                                                      \
    if (!(c)) {                                                                      \
      std::fprintf(stderr, "FAIL %s:%d (%zu nearby rows): %s\n", __FILE__, __LINE__, \
                   (size_t)(rows), #c);                                              \
      std::exit(1);                                                                  \
    }                                                                                \
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
std::vector<uint8_t> witness_of(const Neighbor& n, uint8_t nonce_byte) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, nonce_byte);
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)cc::TPL_INFRA_POWER_OUT;
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
    CHECK_AT(rows, c->suppress_count == v + 1);       // every distinct vote counted
    if (v + 1 < needed) {
      CHECK_AT(rows, !c->suppressed);                 // half or fewer: held
      CHECK_AT(rows, suppressed_lines() == lines_before);
    }
  }
  CHECK_AT(rows, c->suppressed);                      // more than half: suppressed
  CHECK_AT(rows, cc::g_nearby_count == rows);         // the table the votes were counted against
  if (!whole) return;

  CHECK_AT(rows, suppressed_lines() == lines_before + 1);   // logged at the vote that did it
  // The pass after the vote publishes it: the recent route shows it suppressed.
  cc::update();
  CHECK_AT(rows, recent_route_shows(nonce) == 1);
  cc::StatusView s;
  cc::read_status(&s);
  CHECK_AT(rows, s.nearby_count == rows);

  // The rest of the rows vote too: the set holds what it can and the chirp
  // stays suppressed. The set holds at least the most any table asks for.
  const size_t capacity = sizeof(c->suppressed_by) / sizeof(c->suppressed_by[0]);
  CHECK_AT(rows, capacity >= more_than_half(cc::MAX_NEARBY_CACHE));
  for (size_t v = needed; v < rows; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce));
    CHECK_AT(rows, c->suppressed);
    CHECK_AT(rows, c->suppress_count == std::min(v + 1, capacity));
  }
  // The suppression is logged once, at the vote that did it: the votes after
  // it log nothing (every one of them logged the line again, 16 lines for
  // one chirp at 32 rows).
  CHECK_AT(rows, suppressed_lines() == lines_before + 1);

  // Then votes from devices that hold no row, enough to fill the set and
  // more: still suppressed, the set never past its size, and no second line,
  // even from a vote that takes no slot. (Whether such a vote counts at all
  // is F234's; these checks hold either way.)
  for (size_t k = 0; k < capacity + 3; ++k) {
    const Neighbor stranger = neighbor_of((uint8_t)(0x80 + k));
    host_sim::now_ms += 100;
    deliver(stranger, vote_of(stranger, nonce));
    CHECK_AT(rows, c->suppressed);
    CHECK_AT(rows, c->suppress_count <= capacity);
  }
  CHECK_AT(rows, suppressed_lines() == lines_before + 1);

  // The prune drops a suppressed chirp from the table, and the route with it.
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
    CHECK(!c->suppressed);
  }
  host_sim::now_ms += 1000;
  deliver(nearby[0], vote_of(nearby[0], nonce, 0x74));      // the first voter, a fourth time
  CHECK(c->suppress_count == 3);
  CHECK(!c->suppressed);
  deliver(nearby[3], vote_of(nearby[3], nonce));
  CHECK(c->suppress_count == 4);
  CHECK(c->suppressed);

  boot_enabled();
  nearby = fill_nearby(32);
  c = witness_arrives(0x42, nonce);
  for (size_t v = 0; v < 16; ++v) {
    host_sim::now_ms += 500;
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x72));
    deliver(nearby[v], vote_of(nearby[v], nonce, 0x73));
  }
  CHECK(c->suppress_count == 16);
  CHECK(!c->suppressed);
  deliver(nearby[16], vote_of(nearby[16], nonce));
  CHECK(c->suppress_count == 17);
  CHECK(c->suppressed);
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
      CHECK(c->suppress_count == 3);
      CHECK(c->suppressed);
    } else {
      CHECK(c->suppress_count == 2);   // dropped, not stored for later
      CHECK(!c->suppressed);
      // A voter inside the window still counts, and later votes still do not.
      host_sim::now_ms = t0 + 200000;
      deliver(nearby[2], vote_of(nearby[2], nonce, 0x75));
      CHECK(c->suppress_count == 2);
      CHECK(!c->suppressed);
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
    CHECK(a->suppress_count == 0);
    CHECK(!a->suppressed);
    CHECK(b->suppress_count == 4);
    CHECK(b->suppressed);
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
    CHECK(c->suppressed == (late <= 120000u));
    CHECK(c->suppress_count == (late <= 120000u ? 3 : 0));
  }
}

// The same chirp held twice: the nonce filter is cleared whole every
// CHIRP_TTL_MS (300 s), and the freshness window is as long, so a witness
// heard just before a reset and again just after it is stored a second time
// (a NEW item asks that the second copy be refused). A vote is counted at the
// first copy whose window is still open: here 380 s into the scene, 130 s
// after the first copy (closed) and 70 s after the second (open). The first
// copy's closed window ended the search, so the votes landed on neither
// copy (review of F233). With the second copy refused, the votes would fall
// outside the one window there is, and this case changes with that fix.
void test_a_second_copy_takes_its_own_votes() {
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
  advance(60000);   // 310 s: past the filter's reset; the frame is 60 s old
  deliver(kOriginator, w);
  const auto copies = [&]() {
    std::vector<cc::ReceivedChirp*> out;
    for (size_t i = 0; i < cc::g_recent_chirp_count; ++i) {
      if (memcmp(cc::g_recent_chirps[i].nonce, nonce, 8) == 0) out.push_back(&cc::g_recent_chirps[i]);
    }
    return out;
  };
  const std::vector<cc::ReceivedChirp*> held = copies();
  CHECK(held.size() == 2);   // the scene this case is about
  CHECK(held[1]->received_ms - held[0]->received_ms == 60000);

  advance(70000);   // 380 s
  for (const Neighbor& n : nearby) deliver(n, presence_of(n));
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce));
  CHECK(held[0]->suppress_count == 0);   // its window closed at 370 s
  CHECK(!held[0]->suppressed);
  CHECK(held[1]->suppress_count == 3);
  CHECK(held[1]->suppressed);
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
    CHECK(!c->suppressed);
  }
  CHECK(suppressed_lines() == lines_before);

  // The rows' own votes, each signed as the firmware signs one, still count.
  for (size_t v = 0; v < 3; ++v) {
    host_sim::now_ms += 1000;
    deliver(nearby[v], vote_of(nearby[v], nonce));
    CHECK(c->suppress_count == v + 1);
  }
  CHECK(c->suppressed);
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
  CHECK(!c->suppressed);
  for (size_t v = 0; v < 3; ++v) deliver(nearby[v], vote_of(nearby[v], nonce, 0x76));
  CHECK(c->suppressed);
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
    {"a_second_copy_takes_its_own_votes", test_a_second_copy_takes_its_own_votes},
    {"a_vote_must_verify", test_a_vote_must_verify},
    {"a_vote_before_its_chirp_is_not_kept", test_a_vote_before_its_chirp_is_not_kept},
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
