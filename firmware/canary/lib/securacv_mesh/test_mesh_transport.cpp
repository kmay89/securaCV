/**
 * @file test_mesh_transport.cpp
 * @brief Host-build conformance test for the mesh transport.
 *
 * Verifies:
 *   1. Lifecycle: init → start → stop → deinit is idempotent and clean.
 *   2. Peer table: bounded at MESH_TRANSPORT_MAX_PEERS, rejects duplicates,
 *      rejects the broadcast MAC, rolls back on driver failure.
 *   3. Unicast: send_to_peer succeeds to a known peer; fails to unknown.
 *   4. Broadcast: hits every paired peer, NOT the FF MAC.
 *   5. Receive: inject_recv updates last_seen + RSSI; unknown senders drop.
 *   6. Aging: ACTIVE → STALE after 90s → OFFLINE after 5min, with state
 *      callback firing on each transition.
 *   7. Stats: counters move in the right direction.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_transport.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_transport.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_transport && /tmp/test_mesh_transport
 */

#include "mesh_transport.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_transport_run() { return 0; }
#else

using mesh_transport::Peer;
using mesh_transport::PeerState;

namespace {

struct SentFrame {
  uint8_t mac[6];
  std::vector<uint8_t> data;
};
struct RecvFrame {
  uint8_t mac[6];
  std::vector<uint8_t> data;
  int8_t rssi;
};
struct TransitionEvent {
  uint8_t mac[6];
  PeerState old_state;
  PeerState new_state;
};

std::vector<SentFrame> g_sends;
std::vector<RecvFrame> g_recvs;
std::vector<TransitionEvent> g_transitions;
uint32_t g_virtual_now = 0;

bool record_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  SentFrame f; std::memcpy(f.mac, mac, 6);
  f.data.assign(data, data + len);
  g_sends.push_back(std::move(f));
  return true;
}

bool reject_send(const uint8_t*, const uint8_t*, size_t) { return false; }
bool reject_add_peer(const uint8_t*) { return false; }

void record_recv(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi) {
  RecvFrame f; std::memcpy(f.mac, mac, 6);
  f.data.assign(data, data + len);
  f.rssi = rssi;
  g_recvs.push_back(std::move(f));
}

void record_transition(const uint8_t* mac, PeerState old_s, PeerState new_s) {
  TransitionEvent e; std::memcpy(e.mac, mac, 6);
  e.old_state = old_s; e.new_state = new_s;
  g_transitions.push_back(std::move(e));
}

void reset_world() {
  mesh_transport::deinit();
  g_sends.clear();
  g_recvs.clear();
  g_transitions.clear();
  g_virtual_now = 0;
  mesh_transport::test::set_now_ms(0);
  mesh_transport::test::set_send_hook(record_send);
  mesh_transport::test::set_peer_add_hook(nullptr);
  assert(mesh_transport::init(mesh_transport::Config::defaults()));
  mesh_transport::set_recv_callback(record_recv);
  mesh_transport::set_peer_state_callback(record_transition);
  assert(mesh_transport::start());
}

/* ── Test bodies ──────────────────────────────────────────────────────── */

void test_lifecycle() {
  reset_world();
  assert(mesh_transport::is_running());
  mesh_transport::stop();
  assert(!mesh_transport::is_running());
  assert(mesh_transport::start());
  assert(mesh_transport::is_running());
  /* Double-init is a no-op. */
  assert(mesh_transport::init(mesh_transport::Config::defaults()));
  std::printf("PASS test_lifecycle\n");
}

void test_peer_table_bounded() {
  reset_world();
  for (size_t i = 0; i < mesh_transport::MESH_TRANSPORT_MAX_PEERS; ++i) {
    uint8_t m[6] = {0x10, 0, 0, 0, 0, (uint8_t)i};
    assert(mesh_transport::add_peer(m));
  }
  assert(mesh_transport::peer_count() == mesh_transport::MESH_TRANSPORT_MAX_PEERS);

  uint8_t over[6] = {0x10, 0, 0, 0, 0, 0xFE};
  assert(!mesh_transport::add_peer(over));   /* full */

  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  assert(!mesh_transport::add_peer(bcast));  /* broadcast rejected */

  uint8_t dup[6] = {0x10, 0, 0, 0, 0, 0};
  assert(!mesh_transport::add_peer(dup));    /* duplicate */

  std::printf("PASS test_peer_table_bounded\n");
}

void test_add_peer_rolls_back_on_driver_failure() {
  reset_world();
  mesh_transport::test::set_peer_add_hook(reject_add_peer);
  uint8_t m[6] = {0xBE, 0xEF, 0, 0, 0, 1};
  assert(!mesh_transport::add_peer(m));
  assert(mesh_transport::peer_count() == 0);
  assert(!mesh_transport::has_peer(m));
  /* Restore + try again — should now succeed. */
  mesh_transport::test::set_peer_add_hook(nullptr);
  assert(mesh_transport::add_peer(m));
  assert(mesh_transport::peer_count() == 1);
  std::printf("PASS test_add_peer_rolls_back_on_driver_failure\n");
}

void test_unicast_send_to_known_only() {
  reset_world();
  uint8_t a[6] = {0x20, 0, 0, 0, 0, 0x01};
  uint8_t b[6] = {0x20, 0, 0, 0, 0, 0x02};
  assert(mesh_transport::add_peer(a));

  const uint8_t payload[] = {1, 2, 3, 4};
  assert(mesh_transport::send_to_peer(a, payload, sizeof(payload)));
  assert(!mesh_transport::send_to_peer(b, payload, sizeof(payload)));  /* unknown */

  assert(g_sends.size() == 1);
  assert(std::memcmp(g_sends[0].mac, a, 6) == 0);
  assert(g_sends[0].data.size() == 4);
  std::printf("PASS test_unicast_send_to_known_only\n");
}

void test_unicast_send_counters_failure() {
  reset_world();
  uint8_t a[6] = {0x21, 0, 0, 0, 0, 0x01};
  assert(mesh_transport::add_peer(a));
  /* Now flip the send hook to always fail. */
  mesh_transport::test::set_send_hook(reject_send);
  const uint8_t payload[] = {9};
  assert(!mesh_transport::send_to_peer(a, payload, 1));
  mesh_transport::Stats s;
  assert(mesh_transport::get_stats(&s));
  assert(s.unicasts_failed >= 1);
  /* Restore. */
  mesh_transport::test::set_send_hook(record_send);
  std::printf("PASS test_unicast_send_counters_failure\n");
}

void test_broadcast_hits_each_peer() {
  reset_world();
  uint8_t a[6] = {0x30, 0, 0, 0, 0, 0x01};
  uint8_t b[6] = {0x30, 0, 0, 0, 0, 0x02};
  uint8_t c[6] = {0x30, 0, 0, 0, 0, 0x03};
  assert(mesh_transport::add_peer(a));
  assert(mesh_transport::add_peer(b));
  assert(mesh_transport::add_peer(c));

  const uint8_t payload[] = {7, 8};
  const size_t sent = mesh_transport::broadcast(payload, sizeof(payload));
  assert(sent == 3);
  assert(g_sends.size() == 3);
  /* No broadcast-MAC frame should ever be sent. */
  for (const auto& f : g_sends) {
    bool all_ff = true;
    for (int i = 0; i < 6; ++i) if (f.mac[i] != 0xFF) { all_ff = false; break; }
    assert(!all_ff);
  }
  std::printf("PASS test_broadcast_hits_each_peer\n");
}

void test_recv_updates_peer_and_invokes_callback() {
  reset_world();
  uint8_t a[6] = {0x40, 0, 0, 0, 0, 0x01};
  assert(mesh_transport::add_peer(a));
  /* Skip the t=0 add_peer transition log to keep assertions tight. */
  g_transitions.clear();

  mesh_transport::test::set_now_ms(1000);
  const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
  mesh_transport::test::inject_recv(a, payload, sizeof(payload), -55);
  mesh_transport::process();

  assert(g_recvs.size() == 1);
  assert(std::memcmp(g_recvs[0].mac, a, 6) == 0);
  assert(g_recvs[0].data.size() == 3 && g_recvs[0].data[0] == 0xAA);
  assert(g_recvs[0].rssi == -55);

  Peer p;
  assert(mesh_transport::get_peer(a, &p));
  assert(p.last_seen_ms == 1000);
  assert(p.rssi_dbm == -55);
  assert(p.state == PeerState::ACTIVE);
  std::printf("PASS test_recv_updates_peer_and_invokes_callback\n");
}

void test_recv_from_unknown_drops() {
  reset_world();
  uint8_t stranger[6] = {0x50, 0, 0, 0, 0, 0xAA};
  const uint8_t payload[] = {0x11};
  mesh_transport::test::inject_recv(stranger, payload, 1, -60);
  mesh_transport::process();
  assert(g_recvs.empty());
  mesh_transport::Stats s;
  assert(mesh_transport::get_stats(&s));
  assert(s.recv_dropped_no_peer == 1);
  std::printf("PASS test_recv_from_unknown_drops\n");
}

void test_peer_aging_active_stale_offline() {
  reset_world();
  uint8_t a[6] = {0x60, 0, 0, 0, 0, 0x01};
  assert(mesh_transport::add_peer(a));
  g_transitions.clear();

  /* Inject a recv at t=0 to mark last_seen. */
  const uint8_t pay[] = {1};
  mesh_transport::test::inject_recv(a, pay, 1, -50);
  mesh_transport::process();

  /* Advance just shy of stale — peer stays ACTIVE. */
  mesh_transport::test::set_now_ms(mesh_transport::PEER_STALE_AFTER_MS - 1000);
  mesh_transport::process();
  Peer p;
  assert(mesh_transport::get_peer(a, &p));
  assert(p.state == PeerState::ACTIVE);

  /* Cross the stale threshold. */
  mesh_transport::test::set_now_ms(mesh_transport::PEER_STALE_AFTER_MS + 100);
  mesh_transport::process();
  assert(mesh_transport::get_peer(a, &p));
  assert(p.state == PeerState::STALE);

  /* Cross the offline threshold. */
  mesh_transport::test::set_now_ms(mesh_transport::PEER_OFFLINE_AFTER_MS + 100);
  mesh_transport::process();
  assert(mesh_transport::get_peer(a, &p));
  assert(p.state == PeerState::OFFLINE);

  /* State-change callback should have fired twice: ACTIVE→STALE, STALE→OFFLINE. */
  assert(g_transitions.size() == 2);
  assert(g_transitions[0].old_state == PeerState::ACTIVE &&
         g_transitions[0].new_state == PeerState::STALE);
  assert(g_transitions[1].old_state == PeerState::STALE &&
         g_transitions[1].new_state == PeerState::OFFLINE);
  std::printf("PASS test_peer_aging_active_stale_offline\n");
}

void test_recv_restores_active_from_stale() {
  reset_world();
  uint8_t a[6] = {0x70, 0, 0, 0, 0, 0x01};
  assert(mesh_transport::add_peer(a));
  /* Age into STALE. */
  mesh_transport::test::set_now_ms(mesh_transport::PEER_STALE_AFTER_MS + 1);
  mesh_transport::process();
  Peer p;
  assert(mesh_transport::get_peer(a, &p));
  assert(p.state == PeerState::STALE);

  /* A subsequent recv should bounce the peer back to ACTIVE. */
  g_transitions.clear();
  const uint8_t pay[] = {0};
  mesh_transport::test::inject_recv(a, pay, 1, -70);
  mesh_transport::process();
  assert(mesh_transport::get_peer(a, &p));
  assert(p.state == PeerState::ACTIVE);
  assert(g_transitions.size() == 1);
  assert(g_transitions[0].new_state == PeerState::ACTIVE);
  std::printf("PASS test_recv_restores_active_from_stale\n");
}

void test_list_peers_snapshot() {
  reset_world();
  uint8_t a[6] = {0x80, 0, 0, 0, 0, 0x01};
  uint8_t b[6] = {0x80, 0, 0, 0, 0, 0x02};
  assert(mesh_transport::add_peer(a));
  assert(mesh_transport::add_peer(b));

  Peer buf[mesh_transport::MESH_TRANSPORT_MAX_PEERS];
  const size_t n = mesh_transport::list_peers(buf, mesh_transport::MESH_TRANSPORT_MAX_PEERS);
  assert(n == 2);
  std::printf("PASS test_list_peers_snapshot  (count=%zu)\n", n);
}

}  /* namespace */

int main() {
  test_lifecycle();
  test_peer_table_bounded();
  test_add_peer_rolls_back_on_driver_failure();
  test_unicast_send_to_known_only();
  test_unicast_send_counters_failure();
  test_broadcast_hits_each_peer();
  test_recv_updates_peer_and_invokes_callback();
  test_recv_from_unknown_drops();
  test_peer_aging_active_stale_offline();
  test_recv_restores_active_from_stale();
  test_list_peers_snapshot();
  std::printf("\nALL MESH_TRANSPORT TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
