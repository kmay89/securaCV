// Host test for the Canary Link core (firmware/common/link/): frame layout and
// discrimination, nonce construction, the anti-replay window, the send-counter
// lease, session role assignment, the node table's liveness/route rules, and
// the relay's dedupe/ttl decisions.
//
// Builds standalone with g++ — no Arduino, no radio, no crypto library. Run in
// CI by the "Tin Can link core host test" step in .github/workflows/firmware.yml.
// Prints "ALL LINK TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build from the repo root:
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/common/link
//     firmware/projects/canary-tincan/tests_host/test_link_core.cpp -o t && ./t

#include "link_frame.h"
#include "link_node_table.h"
#include "link_relay.h"
#include "link_replay.h"
#include "link_session.h"

#include <cstdio>
#include <cstring>

using namespace canary::link;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void test_frame_roundtrip() {
  uint8_t buf[LINK_MAX_FRAME] = {0};
  const size_t payload = 20;
  CHECK(link_write_header(buf, sizeof(buf), 0x42, 0x1234, Dir::BtoA,
                          0xDEADBEEFCAFEULL, LINK_DEFAULT_TTL, payload),
        "write_header: accepts a legal frame");

  LinkHeader h;
  const size_t total = LINK_HEADER_LEN + payload + LINK_TAG_LEN;
  CHECK(link_parse_header(buf, total, h), "parse_header: round-trips");
  CHECK(h.kind == 0x42, "parse: kind");
  CHECK(h.session_id == 0x1234, "parse: session_id");
  CHECK(h.dir == Dir::BtoA, "parse: dir");
  CHECK(h.ctr == 0xDEADBEEFCAFEULL, "parse: ctr");
  CHECK(h.ttl == LINK_DEFAULT_TTL, "parse: ttl");
  CHECK(h.hops == 0, "parse: hops start at zero");
  CHECK(h.payload_len == payload, "parse: payload length derived from total");
}

// A fleet-link presence beacon must never be mistaken for a link frame. This
// is the discrimination that lets one ESP-NOW receive callback carry both.
static void test_frame_rejects_fleet_beacon() {
  uint8_t beacon[11] = {0xFF, 0xFF, 0x10, 0x01, 0, 0, 0, 0, 0, 0xAB, 0xCD};
  CHECK(!link_is_frame(beacon, sizeof(beacon)),
        "discrimination: an 11-byte fleet beacon is not a link frame");

  LinkHeader h;
  CHECK(!link_parse_header(beacon, sizeof(beacon), h),
        "discrimination: fleet beacon does not parse as a link header");
}

static void test_frame_rejects_malformed() {
  uint8_t buf[LINK_MAX_FRAME] = {0};
  link_write_header(buf, sizeof(buf), 1, 7, Dir::AtoB, 5, LINK_DEFAULT_TTL, 4);
  const size_t total = LINK_HEADER_LEN + 4 + LINK_TAG_LEN;
  LinkHeader h;

  CHECK(link_parse_header(buf, total, h), "sanity: the good frame parses");

  uint8_t bad[LINK_MAX_FRAME];

  std::memcpy(bad, buf, total);
  bad[0] = 0x00;
  CHECK(!link_parse_header(bad, total, h), "reject: wrong magic");

  std::memcpy(bad, buf, total);
  bad[2] = 0x02;
  CHECK(!link_parse_header(bad, total, h), "reject: unknown version");

  // Unknown flag bits are refused rather than ignored: a future field must not
  // be silently discarded by an old build that then acts on a half-understood
  // frame.
  std::memcpy(bad, buf, total);
  bad[7] = 0x01;
  CHECK(!link_parse_header(bad, total, h), "reject: reserved flags set");

  std::memcpy(bad, buf, total);
  bad[6] = 0x02;
  CHECK(!link_parse_header(bad, total, h), "reject: dir out of range");

  CHECK(!link_parse_header(buf, LINK_MIN_FRAME - 1, h),
        "reject: shorter than header + tag");
  CHECK(!link_parse_header(nullptr, total, h), "reject: null buffer");

  CHECK(!link_write_header(buf, sizeof(buf), 1, 7, Dir::AtoB, 5,
                           LINK_DEFAULT_TTL, LINK_MAX_PAYLOAD + 1),
        "reject: payload over the cap");
  CHECK(!link_write_header(buf, sizeof(buf), 1, 7, Dir::AtoB, 5, 0, 4),
        "reject: zero ttl");
  CHECK(!link_write_header(buf, sizeof(buf), 1, 7, Dir::AtoB, 5,
                           LINK_MAX_TTL + 1, 4),
        "reject: ttl over the ceiling");
}

// THE nonce test. Two peers sharing a session and each counting from 1 must
// never produce the same nonce — the failure that destroys both
// confidentiality and authenticity under ChaCha20-Poly1305 and AES-GCM.
static void test_nonce_directions_never_collide() {
  uint8_t a[LINK_NONCE_LEN], b[LINK_NONCE_LEN];
  link_nonce(Dir::AtoB, 0x1234, 1, a);
  link_nonce(Dir::BtoA, 0x1234, 1, b);
  CHECK(std::memcmp(a, b, LINK_NONCE_LEN) != 0,
        "nonce: the two directions' first frames differ");

  // And across every low counter, not just the first.
  for (uint64_t c = 1; c < 256; c++) {
    link_nonce(Dir::AtoB, 0x1234, c, a);
    link_nonce(Dir::BtoA, 0x1234, c, b);
    if (std::memcmp(a, b, LINK_NONCE_LEN) == 0) {
      CHECK(false, "nonce: directional collision at some counter");
      break;
    }
  }

  // Different sessions must not collide either.
  link_nonce(Dir::AtoB, 0x0001, 9, a);
  link_nonce(Dir::AtoB, 0x0002, 9, b);
  CHECK(std::memcmp(a, b, LINK_NONCE_LEN) != 0,
        "nonce: different sessions differ");

  // And the nonce must be recoverable from the frame — deterministic, never
  // random. Same inputs, same bytes, every time.
  link_nonce(Dir::AtoB, 0x1234, 77, a);
  link_nonce(Dir::AtoB, 0x1234, 77, b);
  CHECK(std::memcmp(a, b, LINK_NONCE_LEN) == 0, "nonce: deterministic");

  // The counter must actually reach the nonce, or every frame in a stream
  // would share one.
  link_nonce(Dir::AtoB, 0x1234, 1, a);
  link_nonce(Dir::AtoB, 0x1234, 2, b);
  CHECK(std::memcmp(a, b, LINK_NONCE_LEN) != 0, "nonce: counter varies it");
}

// hops/ttl sit outside the AAD precisely so a relay can rewrite them without
// invalidating an end-to-end tag.
static void test_aad_excludes_mutable_fields() {
  CHECK(LINK_AAD_LEN == 16, "aad: covers the header up to hops");
  CHECK(LINK_AAD_LEN < LINK_HEADER_LEN,
        "aad: stops short of the mutable relay bytes");
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static void test_replay_window() {
  ReplayWindow w;
  CHECK(!w.would_accept(0), "replay: counter 0 is never valid");
  CHECK(w.accept(10), "replay: first frame accepted");
  CHECK(!w.accept(10), "replay: exact repeat refused");
  CHECK(w.accept(11), "replay: next counter accepted");
  CHECK(w.accept(9), "replay: a late but in-window frame is accepted");
  CHECK(!w.accept(9), "replay: that late frame is not accepted twice");

  // Below the floor.
  ReplayWindow w2;
  w2.accept(1000);
  CHECK(!w2.accept(1000 - LINK_REPLAY_WINDOW - 1),
        "replay: below the window floor is refused");
  CHECK(w2.accept(1000 - LINK_REPLAY_WINDOW + 1),
        "replay: just inside the floor is accepted");

  // A big forward jump is legal — a watch that was off for a day is normal.
  ReplayWindow w3;
  w3.accept(5);
  CHECK(w3.accept(5 + 10000), "replay: large forward jump accepted");
  CHECK(!w3.accept(6), "replay: the old range fell off the back");
}

// The boundary. `behind` runs 1..64 and maps to bits 0..63, so a jump of
// EXACTLY the window size still leaves the old `highest` inside the window —
// at the very last slot. Forgetting it there makes a recorded frame replayable
// after nothing more exotic than ordinary packet loss, which on this device
// means a knock that can be played back at 2 a.m.
//
// It also cannot be left to fall out of the shift arithmetic: shifting a
// 64-bit value by 64 is undefined behaviour, so the case needs its own branch.
static void test_replay_window_boundary() {
  const uint64_t n = 1000;

  ReplayWindow at;
  CHECK(at.accept(n), "replay boundary: base counter accepted");
  CHECK(at.accept(n + LINK_REPLAY_WINDOW), "replay boundary: +window accepted");
  CHECK(!at.would_accept(n),
        "replay boundary: the old highest is still remembered at +window");
  CHECK(!at.accept(n), "replay boundary: and refused a second time");

  // One past the window: the old highest legitimately falls off the back.
  ReplayWindow past;
  past.accept(n);
  past.accept(n + LINK_REPLAY_WINDOW + 1);
  CHECK(!past.would_accept(n),
        "replay boundary: past the window, the old counter is below the floor");

  // One inside: unchanged behaviour, guarding the shift path.
  ReplayWindow inside;
  inside.accept(n);
  inside.accept(n + LINK_REPLAY_WINDOW - 1);
  CHECK(!inside.would_accept(n),
        "replay boundary: just inside the window is still remembered");

  // Sweep every jump across the boundary — the old highest must never become
  // acceptable again while it is still inside the window.
  for (uint64_t jump = 1; jump <= LINK_REPLAY_WINDOW; jump++) {
    ReplayWindow w;
    w.accept(n);
    w.accept(n + jump);
    if (w.would_accept(n)) {
      CHECK(false, "replay boundary: some in-window jump forgets the old highest");
      break;
    }
  }
}

// The mirror hazard: a reboot must never rewind the send counter, because that
// reuses (key, nonce). Skipping forward is free; repeating is fatal.
static void test_send_counter_never_rewinds() {
  SendCounter c;
  CHECK(c.needs_persist(), "counter: a fresh counter owes a persist");
  uint64_t v = 0;
  CHECK(!c.take(v), "counter: refuses to send before the lease is durable");

  const uint64_t res = c.new_reservation();
  c.on_persisted(res);
  CHECK(c.take(v) && v == 1, "counter: first value is 1, never 0");
  CHECK(c.take(v) && v == 2, "counter: increments");

  // Simulate an unclean reboot: only the reservation was durable.
  SendCounter after;
  after.restore(res);
  after.on_persisted(after.new_reservation());
  uint64_t v2 = 0;
  CHECK(after.take(v2), "counter: resumes after reboot");
  CHECK(v2 > 2, "counter: resumes ABOVE every value that could have been used");
  CHECK(v2 == res + 1, "counter: resumes from the reservation, not the last use");
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

static void test_role_assignment_is_symmetric() {
  uint8_t lo[LINK_PUBKEY_LEN], hi[LINK_PUBKEY_LEN];
  std::memset(lo, 0x11, sizeof(lo));
  std::memset(hi, 0x99, sizeof(hi));

  bool a_role = false, b_role = false;
  CHECK(link_assign_role(lo, hi, a_role), "role: assigns for distinct keys");
  CHECK(link_assign_role(hi, lo, b_role), "role: assigns from the other side");
  CHECK(a_role, "role: the smaller key is A");
  CHECK(!b_role, "role: the larger key is B");
  CHECK(a_role != b_role, "role: the two peers never agree they are the same");

  // Each peer's send direction is the other's receive direction, with no
  // negotiation round trip.
  CHECK(link_send_dir(a_role) == link_recv_dir(b_role), "role: A->B lines up");
  CHECK(link_send_dir(b_role) == link_recv_dir(a_role), "role: B->A lines up");

  uint8_t same[LINK_PUBKEY_LEN];
  std::memset(same, 0x55, sizeof(same));
  bool ignored = false;
  CHECK(!link_assign_role(same, same, ignored),
        "role: identical keys are refused (self-pair)");
}

static void test_directional_key_labels_differ() {
  CHECK(std::strcmp(LINK_INFO_A_TO_B, LINK_INFO_B_TO_A) != 0,
        "keys: the two HKDF labels are distinct");
  CHECK(std::strcmp(LINK_INFO_KNOT, LINK_INFO_A_TO_B) != 0,
        "keys: the knot label is not a traffic-key label");
  CHECK(std::strcmp(LINK_INFO_KNOT, LINK_INFO_B_TO_A) != 0,
        "keys: the knot label is not a traffic-key label");
  CHECK(std::strcmp(link_info_for_dir(Dir::AtoB), LINK_INFO_A_TO_B) == 0,
        "keys: A->B maps to its own label");
  CHECK(std::strcmp(link_info_for_dir(Dir::BtoA), LINK_INFO_B_TO_A) == 0,
        "keys: B->A maps to its own label");
}

static void test_admission() {
  Session s;
  s.state = SessionState::Active;
  s.session_id = 0x2222;
  s.i_am_a = true;  // I send A->B, I receive B->A

  LinkHeader h;
  h.session_id = 0x2222;
  h.dir = Dir::BtoA;
  h.ctr = 1;
  CHECK(link_admit(s, h), "admit: right session, right direction");

  // A frame carrying my own send direction is a reflection or my traffic being
  // replayed at me. Never legitimate.
  h.dir = Dir::AtoB;
  CHECK(!link_admit(s, h), "admit: refuses my own send direction");

  h.dir = Dir::BtoA;
  h.session_id = 0x3333;
  CHECK(!link_admit(s, h), "admit: refuses a foreign session id");

  h.session_id = 0x2222;
  s.state = SessionState::Pending;
  CHECK(!link_admit(s, h), "admit: refuses a session that isn't active yet");
}

// Revocation is durable and terminal — the surviving peer's own record, which
// is why the operator UI reports a cut as pending until it exists.
static void test_revocation_is_terminal() {
  Session s;
  s.state = SessionState::Active;
  s.session_id = 0x44;
  std::memset(s.key_recv, 0xAB, sizeof(s.key_recv));
  std::memset(s.key_send, 0xCD, sizeof(s.key_send));

  s.revoke();
  CHECK(s.state == SessionState::Revoked, "revoke: state is Revoked");
  CHECK(!s.usable(), "revoke: no longer usable");
  CHECK(s.session_id == 0x44, "revoke: keeps its identity so it can say no");

  uint8_t zero[LINK_KEY_LEN] = {0};
  CHECK(std::memcmp(s.key_recv, zero, LINK_KEY_LEN) == 0,
        "revoke: receive key wiped");
  CHECK(std::memcmp(s.key_send, zero, LINK_KEY_LEN) == 0,
        "revoke: send key wiped");

  LinkHeader h;
  h.session_id = 0x44;
  h.dir = Dir::BtoA;
  h.ctr = 1;
  CHECK(!link_admit(s, h), "revoke: refuses inbound frames forever");

  // A revoked slot is not free — it is still doing a job.
  SessionTable<2> t;
  t.slots[0] = s;
  Session* a = t.alloc();
  CHECK(a == &t.slots[1], "revoke: a revoked slot is not handed out as free");
}

// ---------------------------------------------------------------------------
// Node table
// ---------------------------------------------------------------------------

static void test_node_liveness_is_three_states() {
  NodeTable<4> t;
  Node* n = t.observe(0xAAAA, -50, 0, 0, 1000);
  CHECK(n != nullptr, "nodes: observe creates a node");
  CHECK(n->liveness(1000) == NodeLiveness::Fresh, "nodes: just heard is Fresh");
  CHECK(n->liveness(1000 + NODE_FRESH_MS + 1) == NodeLiveness::Stale,
        "nodes: overdue is Stale, not silently still Fresh");
  CHECK(n->liveness(1000 + NODE_LOST_MS + 1) == NodeLiveness::Lost,
        "nodes: past the horizon is Lost");
  CHECK(!n->reachable(1000 + NODE_LOST_MS + 1), "nodes: Lost is unreachable");
}

// The first RSSI sample seeds the average outright. Folding it against a zero
// initial value would report a wildly optimistic level for the first second —
// which on this device means drawing a taut string that isn't there.
static void test_rssi_first_sample_seeds() {
  CHECK(node_rssi_fold(0, false, -70) == -70, "rssi: first sample seeds");
  const int16_t next = node_rssi_fold(-70, true, -50);
  CHECK(next > -70 && next < -50, "rssi: later samples are smoothed, not jumped");
}

// Equal-length routes must not displace the incumbent, or a house full of
// nodes flaps between two equally good neighbours every beacon.
static void test_route_prefers_shorter_and_ignores_ties() {
  NodeTable<4> t;
  t.observe(0xBEEF, -60, 2, 0x1111, 1000);
  Node* n = t.find(0xBEEF);
  CHECK(n && n->hops == 2 && n->via == 0x1111, "route: initial route recorded");

  t.observe(0xBEEF, -60, 2, 0x2222, 1100);
  CHECK(n->via == 0x1111, "route: an equal-length path does not displace");

  t.observe(0xBEEF, -60, 1, 0x3333, 1200);
  CHECK(n->hops == 1 && n->via == 0x3333, "route: a shorter path wins");
}

// The trap hiding behind "shorter wins, ties don't displace": a node reachable
// through both A and B keeps looking Fresh on B's advertisements while traffic
// is still addressed to a dead A. The node never becomes Lost, so the "is it
// Lost?" failover never fires — and never will. Route freshness therefore has
// to be tracked apart from node freshness.
static void test_route_fails_over_when_the_incumbent_path_goes_quiet() {
  const uint16_t A = 0x1111, B = 0x2222;

  NodeTable<4> t;
  t.observe(0xBEEF, -60, 2, A, 1000);
  Node* n = t.find(0xBEEF);
  CHECK(n && n->via == A, "failover: A is the incumbent");

  // B keeps advertising an equal-length path while A is silent. The node stays
  // Fresh the whole time, which is exactly what used to mask the dead route.
  t.observe(0xBEEF, -60, 2, B, 1000 + NODE_FRESH_MS + 1);
  CHECK(n->liveness(1000 + NODE_FRESH_MS + 1) == NodeLiveness::Fresh,
        "failover: the node itself never looked dead");
  CHECK(n->via == B, "failover: a quiet incumbent is replaced by the live path");

  // And the anti-flap property must survive the fix: while the incumbent IS
  // being heard, an equal-length alternative still does not displace it.
  NodeTable<4> t2;
  t2.observe(0xCAFE, -60, 2, A, 1000);
  Node* m = t2.find(0xCAFE);
  t2.observe(0xCAFE, -60, 2, A, 1000 + NODE_FRESH_MS - 1);  // heard via A
  t2.observe(0xCAFE, -60, 2, B, 1000 + NODE_FRESH_MS + 1);  // B tries
  CHECK(m->via == A, "failover: a live incumbent still wins a tie");

  // An observation through some other next hop must not refresh the
  // incumbent's route clock — that is the bug, stated directly.
  NodeTable<4> t3;
  t3.observe(0xF00D, -60, 2, A, 0);
  Node* k = t3.find(0xF00D);
  for (uint32_t at = 1000; at <= NODE_FRESH_MS; at += 1000) {
    t3.observe(0xF00D, -60, 2, B, at);  // B chatters, A is silent
  }
  t3.observe(0xF00D, -60, 2, B, NODE_FRESH_MS + 1);
  CHECK(k->via == B,
        "failover: B's chatter never kept A's route clock alive");
}

// A full table refuses a new node rather than evicting one that is still
// talking to us.
static void test_node_table_does_not_evict_the_living() {
  NodeTable<2> t;
  t.observe(1, -50, 0, 0, 1000);
  t.observe(2, -50, 0, 0, 1000);
  CHECK(t.observe(3, -50, 0, 0, 1100) == nullptr,
        "nodes: full table of live nodes refuses a newcomer");

  const uint32_t later = 1000 + NODE_LOST_MS + 10;
  Node* n = t.observe(3, -50, 0, 0, later);
  CHECK(n != nullptr, "nodes: a Lost slot may be reclaimed");
  CHECK(t.find(3) != nullptr, "nodes: the newcomer is now present");
}

// ---------------------------------------------------------------------------
// Relay
// ---------------------------------------------------------------------------

static size_t build(uint8_t* buf, uint16_t sid, Dir dir, uint64_t ctr,
                    uint8_t ttl) {
  const size_t payload = 8;
  link_write_header(buf, LINK_MAX_FRAME, 1, sid, dir, ctr, ttl, payload);
  return LINK_HEADER_LEN + payload + LINK_TAG_LEN;
}

static void test_relay_dedupe_and_ttl() {
  RelayCache<8> cache;
  uint8_t buf[LINK_MAX_FRAME] = {0};
  const size_t len = build(buf, 0x77, Dir::AtoB, 42, LINK_DEFAULT_TTL);

  CHECK(link_relay_decide(cache, buf, len, false, 1000) == RelayVerdict::Forward,
        "relay: first sighting is forwarded");
  CHECK(link_relay_decide(cache, buf, len, false, 1010) ==
            RelayVerdict::Duplicate,
        "relay: the same frame by another path is a duplicate");

  // A frame for one of my own sessions is consumed, never relayed.
  CHECK(link_relay_decide(cache, buf, len, true, 1020) == RelayVerdict::Mine,
        "relay: my own session's frame is consumed, not relayed");

  // TTL exhaustion stops the loop.
  uint8_t dying[LINK_MAX_FRAME] = {0};
  const size_t dlen = build(dying, 0x78, Dir::AtoB, 1, 1);
  CHECK(link_relay_decide(cache, dying, dlen, false, 1030) ==
            RelayVerdict::Expired,
        "relay: ttl 1 dies here");

  uint8_t junk[4] = {1, 2, 3, 4};
  CHECK(link_relay_decide(cache, junk, sizeof(junk), false, 1040) ==
            RelayVerdict::Malformed,
        "relay: junk is malformed, not forwarded");
}

// The rewrite must touch exactly the two mutable bytes and nothing else — the
// AEAD tag is computed over the rest and would break.
static void test_relay_rewrite_touches_only_mutable_bytes() {
  uint8_t buf[LINK_MAX_FRAME] = {0};
  const size_t len = build(buf, 0x99, Dir::BtoA, 7, 3);
  uint8_t before[LINK_MAX_FRAME];
  std::memcpy(before, buf, len);

  CHECK(link_relay_rewrite(buf, len), "relay: rewrite succeeds");
  CHECK(buf[16] == 1, "relay: hops incremented");
  CHECK(buf[17] == 2, "relay: ttl decremented");

  CHECK(std::memcmp(before, buf, LINK_AAD_LEN) == 0,
        "relay: the authenticated header prefix is untouched");
  CHECK(std::memcmp(before + LINK_HEADER_LEN, buf + LINK_HEADER_LEN,
                    len - LINK_HEADER_LEN) == 0,
        "relay: the ciphertext and tag are untouched");

  uint8_t dead[LINK_MAX_FRAME] = {0};
  const size_t dlen = build(dead, 0x9A, Dir::AtoB, 1, 1);
  CHECK(!link_relay_rewrite(dead, dlen), "relay: refuses to rewrite a dead ttl");
}

int main() {
  std::printf("Canary Link core host tests\n");

  test_frame_roundtrip();
  test_frame_rejects_fleet_beacon();
  test_frame_rejects_malformed();
  test_nonce_directions_never_collide();
  test_aad_excludes_mutable_fields();

  test_replay_window();
  test_replay_window_boundary();
  test_send_counter_never_rewinds();

  test_role_assignment_is_symmetric();
  test_directional_key_labels_differ();
  test_admission();
  test_revocation_is_terminal();

  test_node_liveness_is_three_states();
  test_rssi_first_sample_seeds();
  test_route_prefers_shorter_and_ignores_ties();
  test_route_fails_over_when_the_incumbent_path_goes_quiet();
  test_node_table_does_not_evict_the_living();

  test_relay_dedupe_and_ttl();
  test_relay_rewrite_touches_only_mutable_bytes();

  if (g_fail) {
    std::printf("\n%d LINK TEST(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL LINK TESTS PASSED\n");
  return 0;
}
