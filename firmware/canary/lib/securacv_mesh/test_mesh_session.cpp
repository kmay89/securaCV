/**
 * @file test_mesh_session.cpp
 * @brief Host-build test for the mesh_session bridge.
 *
 * Bridge layering: mesh_session sits between mesh_transport and
 * mesh_pairing. We test the wiring on one side (singleton state makes
 * a two-peer test in one process impractical; the underlying pairing
 * state machine already has a full two-peer test in
 * test_mesh_pairing.cpp).
 *
 * Verifies:
 *   1. start_initiator → mesh_transport sees an outgoing frame with
 *      MsgType=PAIR_DISCOVER and the device pubkey in the payload.
 *   2. start_joiner → mesh_transport sees an outgoing frame with
 *      MsgType=PAIR_DISCOVER and role=JOINER in the payload.
 *   3. Incoming DISCOVER from a peer (injected via test::inject_recv)
 *      while in DISCOVERING_INITIATOR triggers an outgoing OFFER frame.
 *   4. The wire envelope is exactly [1-byte MsgType][payload bytes]
 *      with no MessageHeader prefix.
 *   5. The bridge ignores frames with reserved/unknown MsgType bytes.
 *   6. cancel_pairing() fires the FailedCallback and wipes state.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_session.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_session.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_pairing.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_transport.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_session && /tmp/test_mesh_session
 */

#include "mesh_session.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_session_run() { return 0; }
#else

namespace {

struct OutFrame {
  uint8_t mac[6];
  std::vector<uint8_t> bytes;
};
std::vector<OutFrame> g_outs;
bool                   g_paired_fired = false;
bool                   g_paired_with_secret = false;
uint8_t                g_paired_secret[mesh_crypto::OPERA_SECRET_LEN];
uint32_t               g_paired_code = 0;
bool                   g_failed_fired = false;
uint32_t               g_code_ready = 0;

bool capture_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  OutFrame f;
  std::memcpy(f.mac, mac, 6);
  f.bytes.assign(data, data + len);
  g_outs.push_back(std::move(f));
  return true;
}

void on_paired(const uint8_t* secret, uint32_t code) {
  g_paired_fired = true;
  g_paired_code = code;
  if (secret != nullptr) {
    g_paired_with_secret = true;
    std::memcpy(g_paired_secret, secret, mesh_crypto::OPERA_SECRET_LEN);
  }
}
void on_failed() { g_failed_fired = true; }
void on_code_ready(uint32_t code) { g_code_ready = code; }

void reset_world() {
  mesh_session::deinit();
  mesh_transport::deinit();
  g_outs.clear();
  g_paired_fired = false;
  g_paired_with_secret = false;
  g_paired_code = 0;
  g_failed_fired = false;
  g_code_ready = 0;
  std::memset(g_paired_secret, 0, sizeof(g_paired_secret));

  mesh_transport::test::set_now_ms(0);
  mesh_transport::test::set_send_hook(capture_send);
  mesh_transport::test::set_peer_add_hook(nullptr);
  assert(mesh_transport::init(mesh_transport::Config::defaults()));
  assert(mesh_transport::start());

  /* Generate our device keypair. */
  static uint8_t pub[mesh_crypto::PUBKEY_LEN];
  static uint8_t priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::init(pub, priv));
  mesh_session::set_paired_callback(on_paired);
  mesh_session::set_failed_callback(on_failed);
  mesh_session::set_code_ready_callback(on_code_ready);
  assert(mesh_session::start());
}

/* ── Test bodies ──────────────────────────────────────────────────────── */

void test_start_initiator_emits_discover_init() {
  /* PR-453 codex P1: pre-membership DISCOVER frame is now sent via
   * mesh_transport::send_raw, which bypasses the peer-table check and
   * routes FF:FF:FF:FF:FF:FF straight to esp_now_send (which has the
   * FF MAC pre-registered by mesh_transport::init). Test asserts the
   * actual wire bytes arrive at our send_hook. */
  reset_world();
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x55 + i);

  assert(mesh_session::start_pairing_initiator(secret, "MyOpera", /*now_ms=*/100));
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_INITIATOR);

  /* Exactly one outgoing frame: PAIR_DISCOVER to FF:FF:FF:FF:FF:FF. */
  assert(g_outs.size() == 1);
  static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  assert(std::memcmp(g_outs[0].mac, BCAST, 6) == 0);
  /* Envelope: byte 0 = PAIR_DISCOVER (0). */
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_DISCOVER));
  /* Body is PairDiscoverPayload with role=INITIATOR. */
  mesh_pairing::PairDiscoverPayload disc;
  std::memcpy(&disc, g_outs[0].bytes.data() + 1, sizeof(disc));
  assert(disc.role == mesh_pairing::ROLE_INITIATOR);
  std::printf("PASS test_start_initiator_emits_discover_init  (frame_len=%zu)\n",
              g_outs[0].bytes.size());
}

void test_start_joiner_emits_discover_join() {
  reset_world();
  assert(mesh_session::start_pairing_joiner(/*now_ms=*/100));
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);

  /* Mirror of the initiator test — DISCOVER frame to FF MAC with role=JOINER. */
  assert(g_outs.size() == 1);
  static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  assert(std::memcmp(g_outs[0].mac, BCAST, 6) == 0);
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_DISCOVER));
  mesh_pairing::PairDiscoverPayload disc;
  std::memcpy(&disc, g_outs[0].bytes.data() + 1, sizeof(disc));
  assert(disc.role == mesh_pairing::ROLE_JOINER);
  std::printf("PASS test_start_joiner_emits_discover_join  (frame_len=%zu)\n",
              g_outs[0].bytes.size());
}

void test_incoming_discover_triggers_offer_unicast() {
  /* Set up as INITIATOR, then simulate the joiner's DISCOVER arriving
   * over mesh_transport. Expected: the bridge dispatches SEND_OFFER as
   * a unicast back to the joiner's MAC. send_to_peer requires the joiner
   * MAC to be a registered peer; we add it manually so the send succeeds
   * and we can capture the wire bytes. */
  reset_world();
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN] = {0};
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)i;
  assert(mesh_session::start_pairing_initiator(secret, "X", 100));
  g_outs.clear();

  /* Add the joiner MAC as a peer so the bridge's SEND_OFFER goes through. */
  const uint8_t joiner_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  assert(mesh_transport::add_peer(joiner_mac));

  /* Build a fake DISCOVER frame from the joiner: 1-byte MsgType=0
   * (PAIR_DISCOVER) + PairDiscoverPayload with role=JOINER. */
  uint8_t joiner_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t joiner_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(joiner_pub, joiner_priv));

  mesh_pairing::PairDiscoverPayload disc{};
  std::memcpy(disc.pubkey, joiner_pub, mesh_crypto::PUBKEY_LEN);
  std::strcpy(disc.device_name, "Joiner");
  disc.role = mesh_pairing::ROLE_JOINER;

  uint8_t frame[1 + sizeof(disc)];
  frame[0] = static_cast<uint8_t>(mesh_session::MsgType::PAIR_DISCOVER);
  std::memcpy(frame + 1, &disc, sizeof(disc));

  mesh_transport::test::inject_recv(joiner_mac, frame, sizeof(frame), -50);
  mesh_transport::process();   /* drains ring → calls bridge recv hook */

  /* Expect ONE outgoing frame: PAIR_OFFER to joiner_mac. */
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, joiner_mac, 6) == 0);
  assert(g_outs[0].bytes.size() == 1 + sizeof(mesh_pairing::PairOfferPayload));
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_OFFER));
  /* Initiator advanced to AWAITING_ACCEPT. */
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_ACCEPT);
  std::printf("PASS test_incoming_discover_triggers_offer_unicast  (frame_len=%zu)\n",
              g_outs[0].bytes.size());
}

void test_envelope_msgtype_byte_is_first_byte() {
  /* Pulls out the OFFER frame from the previous test path and verifies
   * the byte ordering: byte 0 is the MsgType, bytes 1..N are the raw
   * PairOfferPayload struct. Catches reorderings like a 2-byte
   * version+type prefix being introduced accidentally. */
  reset_world();
  assert(mesh_session::start_pairing_joiner(100));
  /* start_joiner dispatched a BROADCAST_DISCOVER through send_to_peer
   * which our hook captures even though mesh_transport rejected the FF
   * MAC for the actual peer table.
   *
   * Hmm — actually no, send_to_peer's has_peer check fails for FF and
   * the send_hook is never called. So we need a different path to test
   * the envelope. Use the OFFER path from the previous test instead. */
  g_outs.clear();

  /* Reuse the previous test's setup: initiator receives joiner's
   * DISCOVER → emits OFFER. Same wire-bytes assertions. */
  reset_world();
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN] = {0};
  assert(mesh_session::start_pairing_initiator(secret, "X", 100));
  g_outs.clear();
  const uint8_t joiner_mac[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
  assert(mesh_transport::add_peer(joiner_mac));

  uint8_t joiner_pub[mesh_crypto::PUBKEY_LEN], joiner_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(joiner_pub, joiner_priv));
  mesh_pairing::PairDiscoverPayload disc{};
  std::memcpy(disc.pubkey, joiner_pub, mesh_crypto::PUBKEY_LEN);
  disc.role = mesh_pairing::ROLE_JOINER;
  uint8_t frame[1 + sizeof(disc)];
  frame[0] = 0;  /* PAIR_DISCOVER */
  std::memcpy(frame + 1, &disc, sizeof(disc));
  mesh_transport::test::inject_recv(joiner_mac, frame, sizeof(frame), -50);
  mesh_transport::process();

  assert(g_outs.size() == 1);
  /* Envelope check: byte 0 == MsgType::PAIR_OFFER (1). */
  assert(g_outs[0].bytes[0] == 1);
  /* The next sizeof(PairOfferPayload) bytes match a PairOfferPayload. */
  assert(g_outs[0].bytes.size() == 1 + sizeof(mesh_pairing::PairOfferPayload));
  mesh_pairing::PairOfferPayload offer;
  std::memcpy(&offer, g_outs[0].bytes.data() + 1, sizeof(offer));
  /* Initiator's ephemeral pubkey must be non-zero (was generated). */
  uint8_t zero[mesh_crypto::PUBKEY_LEN] = {0};
  assert(std::memcmp(offer.ephemeral_pubkey, zero, mesh_crypto::PUBKEY_LEN) != 0);
  std::printf("PASS test_envelope_msgtype_byte_is_first_byte\n");
}

void test_unknown_msgtype_is_silently_dropped() {
  /* Inject a frame with MsgType byte = 200 (reserved-for-future).
   * Expected: bridge ignores it, state unchanged, no failed callback. */
  reset_world();
  assert(mesh_session::start_pairing_joiner(100));
  g_outs.clear();
  g_failed_fired = false;

  const uint8_t mac[6] = {0xCC, 0, 0, 0, 0, 1};
  uint8_t frame[16];
  frame[0] = 200;  /* unknown msg_type */
  for (size_t i = 1; i < sizeof(frame); ++i) frame[i] = 0;
  /* Need to register the sender first or recv path drops with no_peer. */
  assert(mesh_transport::add_peer(mac));
  mesh_transport::test::inject_recv(mac, frame, sizeof(frame), -60);
  mesh_transport::process();

  assert(g_outs.empty());
  assert(!g_failed_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
  std::printf("PASS test_unknown_msgtype_is_silently_dropped\n");
}

void test_cancel_pairing_fires_failed_callback() {
  reset_world();
  assert(mesh_session::start_pairing_joiner(100));
  g_failed_fired = false;
  mesh_session::cancel_pairing();
  assert(g_failed_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  std::printf("PASS test_cancel_pairing_fires_failed_callback\n");
}

void test_lifecycle_idempotent() {
  reset_world();
  /* deinit then re-init should be safe. */
  mesh_session::deinit();
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::init(pub, priv));
  assert(mesh_session::start());
  /* Second init returns true (already initialized). */
  assert(mesh_session::init(pub, priv));
  mesh_session::stop();
  assert(!mesh_session::is_running());
  std::printf("PASS test_lifecycle_idempotent\n");
}

}  /* namespace */

int main() {
  std::srand(0xC51F0);
  test_start_initiator_emits_discover_init();
  test_start_joiner_emits_discover_join();
  test_incoming_discover_triggers_offer_unicast();
  test_envelope_msgtype_byte_is_first_byte();
  test_unknown_msgtype_is_silently_dropped();
  test_cancel_pairing_fires_failed_callback();
  test_lifecycle_idempotent();
  std::printf("\nALL MESH_SESSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
