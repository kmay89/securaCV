/**
 * @file csi_probe_test.cpp
 * @brief Host-build scheduler test for csi_probe.
 *
 * Verifies:
 *   1. With no peers and broadcast_when_no_peers=true, broadcasts fire at
 *      idle_rate_hz (≈1 per 1000/idle_rate_hz ms).
 *   2. With one peer added, unicasts fire at rate_hz; broadcast path
 *      stops firing.
 *   3. Three peers all get serviced (round-robin / per-peer schedule).
 *   4. Wire format: every send carries the "CVP" magic + version=1.
 *   5. Peer table is bounded (CSI_PROBE_MAX_PEERS); add over the cap
 *      fails cleanly.
 *   6. remove_peer() takes effect on the next process() tick.
 *   7. Sequence number is monotonic across sends in a single tick.
 *   8. stop() halts all transmission; start() resumes.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/csi/csi_probe_test.cpp \
 *       firmware/common/csi/src/csi_probe.cpp \
 *       -I firmware/common/csi/src -o /tmp/csi_probe_test && /tmp/csi_probe_test
 */

#include "csi_probe.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int csi_probe_test_run() { return 0; }
#else

namespace {

struct SendRecord {
  uint8_t  mac[csi_probe::CSI_PROBE_MAC_LEN];
  std::vector<uint8_t> payload;
  uint32_t at_ms;
};

std::vector<SendRecord> g_sends;
uint32_t g_virtual_now = 0;

bool record_send(const uint8_t* mac, const uint8_t* payload, size_t len) {
  SendRecord r;
  std::memcpy(r.mac, mac, csi_probe::CSI_PROBE_MAC_LEN);
  r.payload.assign(payload, payload + len);
  r.at_ms = g_virtual_now;
  g_sends.push_back(std::move(r));
  return true;
}

void advance(uint32_t ms_step, uint32_t total_ms) {
  for (uint32_t t = 0; t < total_ms; t += ms_step) {
    g_virtual_now += ms_step;
    csi_probe::test::set_now_ms(g_virtual_now);
    csi_probe::process();
  }
}

bool is_broadcast(const uint8_t mac[csi_probe::CSI_PROBE_MAC_LEN]) {
  for (size_t i = 0; i < csi_probe::CSI_PROBE_MAC_LEN; ++i)
    if (mac[i] != 0xFF) return false;
  return true;
}

void reset_world(const csi_probe::Config& cfg) {
  csi_probe::deinit();
  g_sends.clear();
  g_virtual_now = 0;
  csi_probe::test::set_now_ms(0);
  csi_probe::test::set_send_hook(record_send);
  csi_probe::test::set_peer_add_hook(nullptr);  /* default: driver succeeds */
  assert(csi_probe::init(cfg));
  assert(csi_probe::start());
}

/* ── Test bodies ──────────────────────────────────────────────────────── */

void test_broadcast_idle_rate() {
  csi_probe::Config c = csi_probe::Config::defaults();
  c.rate_hz                = 50;
  c.broadcast_when_no_peers = true;
  c.idle_rate_hz           = 2;       /* one broadcast every 500ms */
  reset_world(c);

  /* Run for 2000ms in 10ms steps → expect 4 broadcasts (t=0,500,1000,1500). */
  advance(10, 2000);

  size_t broadcasts = 0;
  for (const auto& s : g_sends) if (is_broadcast(s.mac)) ++broadcasts;
  assert(broadcasts >= 3 && broadcasts <= 5);
  /* No unicasts should have fired (no peers). */
  for (const auto& s : g_sends) assert(is_broadcast(s.mac));
  std::printf("PASS test_broadcast_idle_rate  (broadcasts=%zu)\n", broadcasts);
}

void test_unicast_rate_one_peer() {
  csi_probe::Config c = csi_probe::Config::defaults();
  /* Defaults: rate_hz=20, aggregate_cap=200. With 1 peer the effective
   * per-peer rate is min(20, 200/1) = 20. */
  c.broadcast_when_no_peers = true;
  reset_world(c);

  uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  assert(csi_probe::add_peer(peer));

  advance(5, 1000);                    /* 1 second */

  size_t unicasts = 0, broadcasts = 0;
  for (const auto& s : g_sends) {
    if (is_broadcast(s.mac)) ++broadcasts; else ++unicasts;
  }
  /* 20 Hz with 5ms ticks → ~20 unicasts/sec. Allow ±3 jitter. */
  assert(unicasts >= 17 && unicasts <= 23);
  /* Broadcasts MUST be zero — we have a peer. */
  assert(broadcasts == 0);
  std::printf("PASS test_unicast_rate_one_peer  (unicasts=%zu, broadcasts=%zu)\n",
              unicasts, broadcasts);
}

void test_three_peers_fair() {
  csi_probe::Config c = csi_probe::Config::defaults();
  /* Defaults give each peer min(20, 200/3) = min(20, 66) = 20 Hz. */
  reset_world(c);

  uint8_t a[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x01};
  uint8_t b[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x02};
  uint8_t cmac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x03};
  assert(csi_probe::add_peer(a));
  assert(csi_probe::add_peer(b));
  assert(csi_probe::add_peer(cmac));

  advance(5, 1000);

  size_t na = 0, nb = 0, nc = 0;
  for (const auto& s : g_sends) {
    if (std::memcmp(s.mac, a, 6) == 0) ++na;
    else if (std::memcmp(s.mac, b, 6) == 0) ++nb;
    else if (std::memcmp(s.mac, cmac, 6) == 0) ++nc;
  }
  /* Each peer should get ~20 sends. Allow ±3 jitter. */
  assert(na >= 17 && na <= 23);
  assert(nb >= 17 && nb <= 23);
  assert(nc >= 17 && nc <= 23);
  std::printf("PASS test_three_peers_fair  (a=%zu b=%zu c=%zu)\n", na, nb, nc);
}

void test_aggregate_cap_with_many_peers() {
  /* The HIGH-severity reviewer concern: with 8 peers, the per-peer rate
   * must be capped by the aggregate budget so the total Tx rate stays
   * bounded. Defaults: rate_hz=20, aggregate_cap_hz=200. 8 peers →
   * effective per-peer = min(20, 200/8) = min(20, 25) = 20. Aggregate
   * = 8 × 20 = 160, comfortably under the 200 cap. */
  csi_probe::Config c = csi_probe::Config::defaults();
  reset_world(c);

  for (size_t i = 0; i < 8; ++i) {
    uint8_t m[6] = {0x77, 0, 0, 0, 0, (uint8_t)i};
    assert(csi_probe::add_peer(m));
  }

  advance(5, 1000);  /* 1 second */
  /* Total sends should be ~160 (8 peers × 20 Hz). */
  assert(g_sends.size() >= 144 && g_sends.size() <= 176);
  std::printf("PASS test_aggregate_cap_with_many_peers  (8 peers, total=%zu)\n",
              g_sends.size());
}

void test_aggregate_cap_throttles_high_rate() {
  /* If the user asks for 100 Hz per peer with 8 peers, the aggregate cap
   * should pull each peer down to 200/8 = 25 Hz (which is then clamped
   * by rate_hz=100, so we get 25 Hz per peer — total 200 Hz). */
  csi_probe::Config c = csi_probe::Config::defaults();
  c.rate_hz = 100;
  c.aggregate_cap_hz = 200;
  reset_world(c);

  for (size_t i = 0; i < 8; ++i) {
    uint8_t m[6] = {0x88, 0, 0, 0, 0, (uint8_t)i};
    assert(csi_probe::add_peer(m));
  }

  advance(5, 1000);
  /* Aggregate capped at 200 Hz total → ~200 sends in 1 sec. */
  assert(g_sends.size() >= 180 && g_sends.size() <= 220);
  std::printf("PASS test_aggregate_cap_throttles_high_rate  (8 peers @ 100Hz cap, total=%zu)\n",
              g_sends.size());
}

void test_wire_format() {
  csi_probe::Config c = csi_probe::Config::defaults();
  c.rate_hz = 100;
  c.broadcast_when_no_peers = true;
  c.idle_rate_hz = 50;
  reset_world(c);

  advance(10, 200);
  assert(!g_sends.empty());
  for (const auto& s : g_sends) {
    assert(s.payload.size() >= sizeof(csi_probe::csi_probe_pkt_t));
    csi_probe::csi_probe_pkt_t pkt;
    std::memcpy(&pkt, s.payload.data(), sizeof(pkt));
    assert(pkt.magic[0] == 'C' && pkt.magic[1] == 'V' && pkt.magic[2] == 'P');
    assert(pkt.version == 1);
    /* Padding must be zero. */
    for (uint8_t p : pkt.padding) assert(p == 0);
    /* Tail bytes (if any) must also be zero. */
    for (size_t i = sizeof(pkt); i < s.payload.size(); ++i)
      assert(s.payload[i] == 0);
  }
  std::printf("PASS test_wire_format  (sends=%zu)\n", g_sends.size());
}

void test_peer_table_bounded() {
  csi_probe::Config c = csi_probe::Config::defaults();
  reset_world(c);

  for (size_t i = 0; i < csi_probe::CSI_PROBE_MAX_PEERS; ++i) {
    uint8_t m[6] = {0x02, 0, 0, 0, 0, (uint8_t)i};
    assert(csi_probe::add_peer(m));
  }
  assert(csi_probe::peer_count() == csi_probe::CSI_PROBE_MAX_PEERS);

  uint8_t over[6] = {0x02, 0, 0, 0, 0, 0xFE};
  assert(!csi_probe::add_peer(over));  /* table full */

  /* Broadcast MAC must be rejected as a peer. */
  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  assert(!csi_probe::add_peer(bcast));

  /* Duplicate add returns false. */
  uint8_t dup[6] = {0x02, 0, 0, 0, 0, 0};
  assert(!csi_probe::add_peer(dup));

  std::printf("PASS test_peer_table_bounded  (count=%zu)\n",
              csi_probe::peer_count());
}

void test_remove_peer_stops_traffic() {
  csi_probe::Config c = csi_probe::Config::defaults();
  c.broadcast_when_no_peers = false;
  reset_world(c);

  uint8_t p[6] = {0x09, 0, 0, 0, 0, 0x01};
  assert(csi_probe::add_peer(p));
  advance(5, 200);
  const size_t before = g_sends.size();
  assert(before > 0);

  assert(csi_probe::remove_peer(p));
  g_sends.clear();
  advance(5, 200);
  /* No peers + broadcast disabled → no sends. */
  assert(g_sends.empty());
  std::printf("PASS test_remove_peer_stops_traffic  (before=%zu after=0)\n",
              before);
}

void test_seq_monotonic() {
  csi_probe::Config c = csi_probe::Config::defaults();
  reset_world(c);

  uint8_t a[6] = {0x0A, 0, 0, 0, 0, 0x01};
  uint8_t b[6] = {0x0A, 0, 0, 0, 0, 0x02};
  assert(csi_probe::add_peer(a));
  assert(csi_probe::add_peer(b));

  advance(5, 100);

  uint32_t prev = 0;
  bool first = true;
  for (const auto& s : g_sends) {
    csi_probe::csi_probe_pkt_t pkt;
    std::memcpy(&pkt, s.payload.data(), sizeof(pkt));
    if (!first) assert(pkt.seq > prev);
    prev = pkt.seq;
    first = false;
  }
  std::printf("PASS test_seq_monotonic  (last_seq=%u)\n", prev);
}

void test_add_peer_rolls_back_on_driver_failure() {
  /* Fix for PR review: add_peer() must not commit a slot if the ESP-NOW
   * driver rejects the peer. Otherwise process() keeps sending to a
   * "registered" peer the driver never accepted, causing persistent
   * send failures while peer_count() reports a phantom entry. */
  csi_probe::Config c = csi_probe::Config::defaults();
  reset_world(c);

  /* Inject a hook that always fails the driver-side registration. */
  csi_probe::test::set_peer_add_hook([](const uint8_t*) { return false; });

  uint8_t p[6] = {0xBE, 0xEF, 0, 0, 0, 0x01};
  assert(!csi_probe::add_peer(p));
  assert(csi_probe::peer_count() == 0);
  assert(!csi_probe::has_peer(p));

  /* Restore default; a subsequent add_peer with success should now
   * register cleanly. */
  csi_probe::test::set_peer_add_hook(nullptr);
  assert(csi_probe::add_peer(p));
  assert(csi_probe::peer_count() == 1);
  std::printf("PASS test_add_peer_rolls_back_on_driver_failure\n");
}

void test_initial_sends_staggered() {
  /* Fix for PR review: peers registered before start() must NOT all
   * transmit on the same tick — the round-robin claim is meaningless
   * otherwise. Phase distribution should spread the first send across
   * one period. */
  csi_probe::Config c = csi_probe::Config::defaults();
  /* Defaults: rate_hz=20, period=50ms. With 4 peers, phases at
   * 0, 12, 25, 37 ms. */
  reset_world(c);
  csi_probe::stop();
  csi_probe::clear_peers();

  uint8_t a[6] = {0x44, 0, 0, 0, 0, 0x01};
  uint8_t b[6] = {0x44, 0, 0, 0, 0, 0x02};
  uint8_t cc[6] = {0x44, 0, 0, 0, 0, 0x03};
  uint8_t d[6] = {0x44, 0, 0, 0, 0, 0x04};
  assert(csi_probe::add_peer(a));
  assert(csi_probe::add_peer(b));
  assert(csi_probe::add_peer(cc));
  assert(csi_probe::add_peer(d));

  g_sends.clear();
  g_virtual_now = 0;
  csi_probe::test::set_now_ms(0);
  csi_probe::start();

  /* First tick at t=0: only the j=0 peer (phase=0) should fire. */
  csi_probe::process();
  assert(g_sends.size() == 1);

  /* Phases for 4 peers at period=50ms are 0, 12, 25, 37. Advance just
   * shy of the second cycle (t=40, before peer 0's second send at t=50)
   * — each peer should have fired exactly once. */
  advance(1, 40);
  assert(g_sends.size() == 4);

  /* And those 4 sends should be on distinct MACs (one per peer). */
  bool seen_a = false, seen_b = false, seen_c = false, seen_d = false;
  for (const auto& s : g_sends) {
    if (std::memcmp(s.mac, a, 6) == 0) seen_a = true;
    else if (std::memcmp(s.mac, b, 6) == 0) seen_b = true;
    else if (std::memcmp(s.mac, cc, 6) == 0) seen_c = true;
    else if (std::memcmp(s.mac, d, 6) == 0) seen_d = true;
  }
  assert(seen_a && seen_b && seen_c && seen_d);
  std::printf("PASS test_initial_sends_staggered  (4 peers, 1st tick=1 send, 1st period=4)\n");
}

void test_stop_halts_sends() {
  csi_probe::Config c = csi_probe::Config::defaults();
  c.broadcast_when_no_peers = true;
  c.idle_rate_hz = 10;
  reset_world(c);

  advance(5, 200);
  const size_t before = g_sends.size();
  assert(before > 0);

  csi_probe::stop();
  g_sends.clear();
  advance(5, 200);
  assert(g_sends.empty());

  csi_probe::start();
  advance(5, 200);
  assert(!g_sends.empty());
  std::printf("PASS test_stop_halts_sends  (before=%zu, after_stop=0, after_resume=%zu)\n",
              before, g_sends.size());
}

}  /* namespace */

int main() {
  test_broadcast_idle_rate();
  test_unicast_rate_one_peer();
  test_three_peers_fair();
  test_aggregate_cap_with_many_peers();
  test_aggregate_cap_throttles_high_rate();
  test_wire_format();
  test_peer_table_bounded();
  test_remove_peer_stops_traffic();
  test_seq_monotonic();
  test_add_peer_rolls_back_on_driver_failure();
  test_initial_sends_staggered();
  test_stop_halts_sends();
  std::printf("\nALL CSI_PROBE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
