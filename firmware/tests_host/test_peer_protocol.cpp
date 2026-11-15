// Host tests for firmware/common/network/peer_protocol.h — the frame bytes
// and tag, the replay window, the table, the coordinator rule.

#include "../common/network/peer_protocol.h"

#include <cstdio>
#include <cstring>

using namespace canary::net::peer;
namespace fk = canary::net::fleet_key;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static uint8_t KEY[fk::KEY_BYTES];
static uint8_t OTHER_KEY[fk::KEY_BYTES];

static Frame heartbeat_from(uint8_t fp0, uint32_t uptime, uint32_t seq, bool hub_ok = false) {
  Frame f;
  f.role = ROLE_WITNESS;
  f.flags = hub_ok ? FLAG_HUB_OK : 0;
  std::memset(f.fp, 0, FP_BYTES); f.fp[0] = fp0; f.fp[7] = 0xaa;
  f.uptime_s = uptime; f.seq = seq;
  Heartbeat h; h.onboarding = 3; h.hub = hub_ok ? 3 : 1; h.ota = 0; h.rssi = -61; h.caps = 0x0023; h.peers_seen = 2; h.saved_networks = 1;
  put_heartbeat(f, h);
  return f;
}

static void the_frame_round_trips() {
  Frame f = heartbeat_from(0x11, 3600, 42);
  f.coord_fp[0] = 0x11;
  f.boot_id = 0x5a;
  uint8_t buf[MAX_FRAME];
  const size_t n = encode(f, KEY, buf, sizeof(buf));
  CHECK(n == HEADER_BYTES + HEARTBEAT_PAYLOAD + TAG_BYTES, "size %zu", n);
  CHECK(buf[0] == 'S' && buf[1] == 'C' && buf[2] == 'V' && buf[3] == 'P' && buf[4] == 1, "magic + version");
  CHECK(buf[5] == 1 && buf[7] == ROLE_WITNESS && buf[32] == HEARTBEAT_PAYLOAD && buf[33] == 0x5a, "header fields (boot id at 33)");
  CHECK(buf[16] == 0x10 && buf[17] == 0x0e && buf[18] == 0 && buf[19] == 0, "uptime little-endian");
  Frame back;
  CHECK(decode(buf, n, KEY, back) == Verdict::Ok, "decodes");
  CHECK(back.type == Type::Heartbeat && back.uptime_s == 3600 && back.seq == 42 && back.fp[0] == 0x11 && back.fp[7] == 0xaa, "fields back");
  CHECK(back.coord_fp[0] == 0x11 && back.boot_id == 0x5a, "coord and boot id back");
  uint8_t tampered[MAX_FRAME]; std::memcpy(tampered, buf, n); tampered[33] = 0;
  CHECK(decode(tampered, n, KEY, back) == Verdict::BadTag, "the boot id is under the tag");
  Frame older = heartbeat_from(0x11, 3600, 42);
  CHECK(encode(older, KEY, buf, sizeof(buf)) == n && buf[33] == 0, "a sender without one sends 0");
  Heartbeat h;
  CHECK(get_heartbeat(back, h), "heartbeat payload");
  CHECK(h.onboarding == 3 && h.hub == 1 && h.rssi == -61 && h.caps == 0x0023 && h.peers_seen == 2 && h.saved_networks == 1, "heartbeat fields");
  uint8_t small[40];
  CHECK(encode(f, KEY, small, sizeof(small)) == 0, "too small: 0");
}

static void the_event_round_trips() {
  Frame f; f.fp[0] = 0x22; f.seq = 1; f.uptime_s = 10;
  Event e; e.kind = EV_DETECT_EDGE; e.object_class = 1; e.score = 87; e.bucket = 77;
  put_event(f, e);
  uint8_t buf[MAX_FRAME];
  const size_t n = encode(f, KEY, buf, sizeof(buf));
  CHECK(n == HEADER_BYTES + EVENT_PAYLOAD + TAG_BYTES, "size");
  Frame back; Event eb;
  CHECK(decode(buf, n, KEY, back) == Verdict::Ok && get_event(back, eb), "decodes");
  CHECK(eb.kind == EV_DETECT_EDGE && eb.object_class == 1 && eb.score == 87 && eb.bucket == 77, "event fields");
  CHECK(!get_heartbeat(back, *(new Heartbeat)), "an event is not a heartbeat");
}

static void only_the_paired_verify() {
  Frame f = heartbeat_from(0x11, 1, 1);
  uint8_t buf[MAX_FRAME];
  const size_t n = encode(f, KEY, buf, sizeof(buf));
  Frame back;
  CHECK(decode(buf, n, OTHER_KEY, back) == Verdict::BadTag, "another household's key");
  uint8_t tampered[MAX_FRAME]; std::memcpy(tampered, buf, n); tampered[HEADER_BYTES] ^= 0x01;
  CHECK(decode(tampered, n, KEY, back) == Verdict::BadTag, "a flipped payload bit");
  std::memcpy(tampered, buf, n); tampered[0] = 'X';
  CHECK(decode(tampered, n, KEY, back) == Verdict::BadMagic, "not ours");
  std::memcpy(tampered, buf, n); tampered[4] = 2;
  CHECK(decode(tampered, n, KEY, back) == Verdict::BadVersion, "a future version");
  CHECK(decode(buf, n - 1, KEY, back) == Verdict::BadLength, "a byte short of its own length");
  CHECK(decode(buf, MIN_FRAME - 1, KEY, back) == Verdict::TooShort, "shorter than any frame");
  std::memcpy(tampered, buf, n); tampered[32] = 100;
  CHECK(decode(tampered, n, KEY, back) == Verdict::BadLength, "an overlong payload claim");
  // The beacon's own bytes on the same group are not a frame.
  const uint8_t beacon[11] = {0xFF, 0xFF, 0x10, 0x01, 0, 0xFF, 0xFF, 0, 0, 0xab, 0xcd};
  CHECK(decode(beacon, sizeof(beacon), KEY, back) == Verdict::TooShort, "a presence beacon is rejected");
  CHECK(!std::strcmp(verdict_label(Verdict::BadTag), "bad_tag"), "label");
}

static void the_table_and_the_replay_window() {
  Table t;
  uint8_t self[FP_BYTES] = {0x01, 0, 0, 0, 0, 0, 0, 0xaa};
  Frame a = heartbeat_from(0x11, 100, 5);
  CHECK(accept(t, a, 1000, 0xC0A80105, self) == Verdict::Ok, "first frame");
  CHECK(count(t) == 1, "one peer");
  CHECK(t.peers[0].ip == 0, "a first sight names no address (the tag does not cover it)");
  CHECK(accept(t, a, 1500, 0xC0A80105, self) == Verdict::Replay, "the same seq again");
  Frame a2 = heartbeat_from(0x11, 110, 4);
  CHECK(accept(t, a2, 2000, 0, self) == Verdict::Replay, "an older seq with the uptime still climbing");
  Frame a3 = heartbeat_from(0x11, 110, 6);
  CHECK(accept(t, a3, 2500, 0xC0A80105, self) == Verdict::Ok, "the next seq");
  CHECK(t.peers[0].ip == 0xC0A80105, "an in-order frame names the address");
  Frame a4 = heartbeat_from(0x11, 120, 7);
  CHECK(accept(t, a4, 2600, 0, self) == Verdict::Ok && t.peers[0].ip == 0xC0A80105, "an unknown address keeps the known one");
  Frame rebooted = heartbeat_from(0x11, 3, 1);
  CHECK(accept(t, rebooted, 3000, 0, self) == Verdict::Ok, "a reboot (uptime fell) is heard");
  CHECK(t.peers[0].last_seq == 7 && t.peers[0].uptime_s == 120 && t.peers[0].pending, "provisionally: the counter waits for its second frame");
  Frame me = heartbeat_from(0x01, 50, 9);
  CHECK(accept(t, me, 3100, 0, self) == Verdict::Self, "our own echo is dropped");
  CHECK(count(t) == 1, "still one peer");
  for (uint8_t i = 0; i < MAX_PEERS - 1; i++) {
    Frame p = heartbeat_from((uint8_t)(0x30 + i), 10, 1);
    CHECK(accept(t, p, 4000, 0, self) == Verdict::Ok, "peer %u", i);
  }
  CHECK(count(t) == (int)MAX_PEERS, "table full");
  Frame extra = heartbeat_from(0x7f, 10, 1);
  CHECK(accept(t, extra, 4000, 0, self) == Verdict::Full, "a seventeenth is refused");
  // Bring the first peer up to the same clock so the boundary is one instant.
  Frame fresh = heartbeat_from(0x11, 4, 2);
  CHECK(accept(t, fresh, 4000, 0, self) == Verdict::Ok, "refresh: the reboot's second frame confirms it");
  CHECK(t.peers[0].last_seq == 2 && t.peers[0].uptime_s == 4 && !t.peers[0].pending, "table follows");
  expire(t, 4000 + STALE_MS, STALE_MS);
  CHECK(count(t) == (int)MAX_PEERS, "exactly at the limit: kept");
  expire(t, 4000 + STALE_MS + 1, STALE_MS);
  CHECK(count(t) == 0, "past it: gone");
  CHECK(STALE_MS == 35000 && HEARTBEAT_MS == 10000, "three missed heartbeats");
}

// A frame is one of two kinds here: `live` is the peer itself (address A),
// `captured` is the same bytes sent again later from the attacker's address
// X. The tag verifies either way; accept() has to tell them apart.
static Frame keyed_from(uint8_t boot_id, uint32_t uptime, uint32_t seq) {
  Frame f = heartbeat_from(0x11, uptime, seq);
  f.boot_id = boot_id;
  return f;
}

static void a_captured_frame_is_not_a_reboot() {
  const uint32_t A = 0xC0A80105, X = 0xC0A800FE;
  uint8_t self[FP_BYTES] = {0x01, 0, 0, 0, 0, 0, 0, 0xaa};

  // ── Senders without a boot id (the fallback) ──
  Table t;
  CHECK(accept(t, heartbeat_from(0x11, 4990, 399), 0, A, self) == Verdict::Ok, "first sight");
  CHECK(accept(t, heartbeat_from(0x11, 5000, 400), 10000, A, self) == Verdict::Ok, "in order");
  Peer& p = t.peers[0];
  CHECK(p.ip == A && p.last_seq == 400 && p.uptime_s == 5000, "the row");
  // Captured mid-boot frames: uptime fell, but far past a boot's first frames.
  CHECK(accept(t, heartbeat_from(0x11, 2000, 150), 11000, X, self) == Verdict::Replay, "a lower uptime past the seq window is a replay");
  CHECK(accept(t, heartbeat_from(0x11, 130, 2), 11000, X, self) == Verdict::Replay, "a low seq past the uptime window is a replay");
  CHECK(accept(t, heartbeat_from(0x11, 90, 9), 11000, X, self) == Verdict::Replay, "seq 9 is past the window of 8");
  CHECK(REBOOT_SEQ_WINDOW == 8 && REBOOT_UPTIME_WINDOW_S == 120 && REBOOT_MIN_INTERVAL_MS == 30000, "the windows");
  // A captured first-boot frame: inside the window, so it is heard once —
  // and moves nothing but presence.
  CHECK(accept(t, heartbeat_from(0x11, 3, 1), 12000, X, self) == Verdict::Ok, "a captured first-boot frame is heard once");
  CHECK(p.ip == A && p.last_seq == 400 && p.uptime_s == 5000 && p.pending && p.last_seen_ms == 12000,
        "the address, the counter and the uptime are not re-pointed by it");
  CHECK(accept(t, heartbeat_from(0x11, 3, 1), 12500, X, self) == Verdict::Replay, "the same frame again is a replay");
  CHECK(accept(t, heartbeat_from(0x11, 5, 1), 13000, X, self) == Verdict::Replay, "another claimed reboot inside the interval is a replay");
  // The live peer carries on as if nothing happened: in order, same address.
  CHECK(accept(t, heartbeat_from(0x11, 5010, 401), 20000, A, self) == Verdict::Ok, "the live peer's next frame");
  CHECK(p.ip == A && p.last_seq == 401 && !p.pending, "the provisional sighting is dropped");
  // A genuine reboot: first frame provisional, second confirms, address then follows.
  CHECK(accept(t, heartbeat_from(0x11, 3, 1), 60000, A, self) == Verdict::Ok, "a genuine reboot, past the interval");
  CHECK(p.last_seq == 401 && p.uptime_s == 5010, "still provisional");
  CHECK(accept(t, heartbeat_from(0x11, 13, 2), 70000, 0xC0A80106, self) == Verdict::Ok, "its second frame");
  CHECK(p.last_seq == 2 && p.uptime_s == 13 && p.ip == 0xC0A80106 && !p.pending, "confirmed: counter, uptime and (new) address follow");
  // The fallback's residual: without a boot id, an earlier boot's frame with
  // a higher counter than the row's is in order, and names the address. The
  // keyed window below closes this; it stands only for a sender that predates it.
  CHECK(accept(t, heartbeat_from(0x11, 5010, 401), 71000, X, self) == Verdict::Ok && p.ip == X,
        "fallback residual: an earlier boot's higher counter passes for a sender without a boot id");

  // ── Senders with a boot id: the window is keyed on (boot_id, seq) ──
  Table k;
  CHECK(accept(k, keyed_from(0x5a, 4990, 399), 0, A, self) == Verdict::Ok, "first sight");
  CHECK(accept(k, keyed_from(0x5a, 5000, 400), 10000, A, self) == Verdict::Ok, "in order");
  Peer& q = k.peers[0];
  CHECK(q.boot_id == 0x5a && q.ip == A, "keyed row");
  CHECK(accept(k, keyed_from(0x5a, 3, 1), 11000, X, self) == Verdict::Replay,
        "a captured first frame of the SAME boot is a replay outright (never even provisional)");
  CHECK(!q.pending, "not provisional");
  CHECK(accept(k, keyed_from(0x5a, 3, 401), 11000, A, self) == Verdict::Ok, "within a boot only the counter counts");
  CHECK(accept(k, keyed_from(0x11, 3, 1), 12000, X, self) == Verdict::Ok, "a captured first frame of an OLDER boot is heard once");
  CHECK(q.boot_id == 0x5a && q.ip == A && q.last_seq == 401 && q.pending && q.pend_boot_id == 0x11, "and moves nothing");
  CHECK(accept(k, keyed_from(0x11, 13, 2), 13000, X, self) == Verdict::Ok, "its captured second frame confirms that boot");
  CHECK(q.boot_id == 0x11 && q.prev_boot_id == 0x5a && q.prev_last_seq == 401 && q.ip == X,
        "a captured PAIR the receiver never saw re-points the row (the residual, docs/TRUST_MODEL.md)");
  CHECK(accept(k, keyed_from(0x5a, 5020, 402), 14000, A, self) == Verdict::Ok,
        "the live peer's next frame is from the boot before, past where it was left: taken");
  CHECK(q.boot_id == 0x5a && q.prev_boot_id == 0x11 && q.prev_last_seq == 2 && q.ip == A && q.last_seq == 402,
        "the row swaps back and the address is the live peer's again");
  CHECK(accept(k, keyed_from(0x11, 13, 2), 15000, X, self) == Verdict::Replay, "the captured pair is spent");
  CHECK(accept(k, keyed_from(0x11, 23, 3), 15000, X, self) == Verdict::Ok && q.boot_id == 0x11 && q.ip == X,
        "a third captured frame of that boot, never seen here: taken once (each buys the row until the live peer's next frame)");
  CHECK(accept(k, keyed_from(0x5a, 5030, 403), 16000, A, self) == Verdict::Ok && q.boot_id == 0x5a && q.ip == A,
        "which takes the row back");
  CHECK(accept(k, keyed_from(0x11, 23, 3), 16000, X, self) == Verdict::Replay, "that frame is spent too");
  CHECK(accept(k, keyed_from(0x11, 23, 3), 50000, X, self) == Verdict::Replay && !q.pending,
        "and a seen frame of a boot the row knows is never a reboot, however long the interval");
  CHECK(accept(k, keyed_from(0x11, 3, 1), 50000, X, self) == Verdict::Replay && !q.pending, "not even its first frame");
  // A genuine reboot with a new boot id: once, then keyed on it.
  CHECK(accept(k, keyed_from(0x7c, 3, 1), 60000, A, self) == Verdict::Ok, "a new boot id, first frame");
  CHECK(q.boot_id == 0x5a && q.pending, "provisional");
  CHECK(accept(k, keyed_from(0x7c, 13, 2), 70000, A, self) == Verdict::Ok, "second frame");
  CHECK(q.boot_id == 0x7c && q.last_seq == 2 && q.prev_boot_id == 0x5a && q.prev_last_seq == 403, "confirmed and keyed");
  CHECK(accept(k, keyed_from(0x7c, 23, 3), 80000, A, self) == Verdict::Ok, "in order under the new boot id");
  CHECK(accept(k, keyed_from(0x5a, 5000, 400), 81000, X, self) == Verdict::Replay, "every seen frame of the boot before is a replay");
  CHECK(accept(k, keyed_from(0x5a, 3, 1), 81000, X, self) == Verdict::Replay, "its first frame too");
  CHECK(accept(k, keyed_from(0x7c, 3, 1), 81000, X, self) == Verdict::Replay, "and the new boot's first frame, captured, is a replay");
  CHECK(accept(k, keyed_from(0x7c, 33, 4), 90000, A, self) == Verdict::Ok && q.last_seq == 4, "the live peer carries on");
}

static void the_oldest_coordinates_and_a_hub_wins() {
  Table t;
  uint8_t self[FP_BYTES] = {0x50, 0, 0, 0, 0, 0, 0, 0};
  Election e = elect(t, self, 1000, false, 0);
  CHECK(e.who == Election::Who::Self && e.fp[0] == 0x50, "alone: self");
  char w[17];
  CHECK(coord_word(e, w, sizeof(w)) == 4 && !std::strcmp(w, "self"), "word self");
  Frame young = heartbeat_from(0x10, 500, 1);
  accept(t, young, 10000, 0, self);
  e = elect(t, self, 1000, false, 10000);
  CHECK(e.who == Election::Who::Self, "a younger peer does not coordinate");
  Frame old = heartbeat_from(0x90, 5000, 1);
  accept(t, old, 10000, 0, self);
  e = elect(t, self, 1000, false, 10000);
  CHECK(e.who == Election::Who::Peer && e.fp[0] == 0x90, "the oldest peer coordinates");
  CHECK(coord_word(e, w, sizeof(w)) == 16 && !std::strncmp(w, "90000000000000aa", 16), "word is its fingerprint: %s", w);
  // Aging: a peer heard 30 s ago is credited those 30 s.
  e = elect(t, self, 5020, false, 40000);
  CHECK(e.who == Election::Who::Peer && e.fp[0] == 0x90, "aged peer 5000+30 beats our 5020");
  e = elect(t, self, 5031, false, 40000);
  CHECK(e.who == Election::Who::Self, "we are older than the aged peer");
  // Tie → lowest fingerprint.
  Table t2;
  Frame tie = heartbeat_from(0x20, 1000, 1);
  accept(t2, tie, 0, 0, self);
  e = elect(t2, self, 1000, false, 0);
  CHECK(e.who == Election::Who::Peer && e.fp[0] == 0x20, "tie: lower fingerprint (0x20 < 0x50)");
  uint8_t self_low[FP_BYTES] = {0x10, 0, 0, 0, 0, 0, 0, 0};
  e = elect(t2, self_low, 1000, false, 0);
  CHECK(e.who == Election::Who::Self, "tie: we are lower");
  // A hub anywhere wins; every node yields.
  e = elect(t2, self_low, 1000, true, 0);
  CHECK(e.who == Election::Who::Hub, "our own hub");
  CHECK(coord_word(e, w, sizeof(w)) == 3 && !std::strcmp(w, "hub"), "word hub");
  Frame on_hub = heartbeat_from(0x05, 1, 1, /*hub_ok=*/true);
  accept(t2, on_hub, 0, 0, self_low);
  e = elect(t2, self_low, 99999, false, 0);
  CHECK(e.who == Election::Who::Hub, "a peer on its hub: everyone yields, however old we are");
  // Hub gone from every heartbeat: the rule elects again.
  Frame off_hub = heartbeat_from(0x05, 2, 2, false);
  accept(t2, off_hub, 100, 0, self_low);
  e = elect(t2, self_low, 99999, false, 100);
  CHECK(e.who == Election::Who::Self, "hub lost: back to oldest-wins");
}

int main() {
  std::memset(KEY, 0x42, sizeof(KEY));
  std::memset(OTHER_KEY, 0x24, sizeof(OTHER_KEY));
  the_frame_round_trips();
  the_event_round_trips();
  only_the_paired_verify();
  the_table_and_the_replay_window();
  a_captured_frame_is_not_a_reboot();
  the_oldest_coordinates_and_a_hub_wins();
  if (g_failures) { std::printf("test_peer_protocol: %d failure(s)\n", g_failures); return 1; }
  std::printf("test_peer_protocol: all passed\n");
  return 0;
}
