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
#include "mesh_envelope.h"
#include "mesh_beacon.h"
#include "mesh_api.h"
#include "mesh_state.h"
#include "mesh_alert.h"
#include "mesh_rekey.h"

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
  std::strncpy(disc.device_name, "Joiner", sizeof(disc.device_name) - 1);
  disc.device_name[sizeof(disc.device_name) - 1] = '\0';
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

/* ────────────────────────────────────────────────────────────────────────
 * PR 5c-3 — send_beacon_event
 * ──────────────────────────────────────────────────────────────────────── */

void test_send_beacon_event_rejected_without_opera_secret() {
  reset_world();

  /* No set_opera_secret() call → send must fail. */
  assert(!mesh_session::has_opera_secret());
  assert(!mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                          "kitchen", /*now_ms=*/1000));
  assert(g_outs.empty());
  std::printf("PASS test_send_beacon_event_rejected_without_opera_secret\n");
}

void test_send_beacon_event_signs_and_broadcasts() {
  reset_world();

  /* Pull the device pubkey/privkey out of reset_world's static state
   * by regenerating one locally and re-init'ing the session. We need
   * pub for parse_and_verify and for compute_fingerprint comparison. */
  mesh_session::deinit();
  uint8_t pub[mesh_crypto::PUBKEY_LEN];
  uint8_t priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::init(pub, priv));
  assert(mesh_session::start());
  g_outs.clear();

  /* Provide an opera_secret + add a peer so the broadcast has a target. */
  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(i + 1);
  assert(mesh_session::set_opera_secret(opera_secret));
  assert(mesh_session::has_opera_secret());

  /* Add a paired peer so mesh_transport::broadcast has someone to send to. */
  uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  assert(mesh_transport::add_peer(peer_mac));

  /* Send. */
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                         "kitchen", /*now_ms=*/12345));

  /* One frame should have been captured by the send hook. */
  assert(g_outs.size() == 1);
  const auto& f = g_outs[0];

  /* Wire shape: [session_msg_type=22 (1B)] [Header(38B)] [Payload(25B)] [Sig(64B)]
   * = 128 bytes total. */
  const size_t expected_len = 1
      + mesh_envelope::HEADER_LEN
      + mesh_beacon::PAYLOAD_LEN
      + mesh_envelope::SIGNATURE_LEN;
  assert(f.bytes.size() == expected_len);
  assert(f.bytes[0] == static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT));

  /* Verify the signed envelope (frame minus the leading session byte). */
  mesh_envelope::Header  hdr;
  const uint8_t*         payload = nullptr;
  size_t                 payload_len = 0;
  assert(mesh_envelope::parse_and_verify(
      f.bytes.data() + 1, f.bytes.size() - 1,
      pub, &hdr, &payload, &payload_len));
  assert(hdr.version  == mesh_envelope::PROTOCOL_VERSION);
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT));
  assert(payload_len  == mesh_beacon::PAYLOAD_LEN);

  /* sender_fp matches our pubkey's fingerprint. */
  uint8_t expected_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(pub, expected_fp);
  assert(std::memcmp(hdr.sender_fp, expected_fp, sizeof(expected_fp)) == 0);

  /* opera_id matches the derivation from the secret we provided. */
  uint8_t expected_oid[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(opera_secret, expected_oid);
  assert(std::memcmp(hdr.opera_id, expected_oid, sizeof(expected_oid)) == 0);

  /* Counter is monotonic and starts at 1 on first send. */
  assert(hdr.counter == 1);

  /* timestamp matches what the caller passed in. */
  assert(hdr.timestamp == 12345);

  /* Payload decodes to (ARRIVED, "kitchen"). */
  mesh_beacon::BeaconState got_state;
  char                     got_label[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(mesh_beacon::decode(payload, payload_len,
                             &got_state, got_label, sizeof(got_label)));
  assert(got_state == mesh_beacon::BeaconState::ARRIVED);
  assert(std::strcmp(got_label, "kitchen") == 0);

  std::printf("PASS test_send_beacon_event_signs_and_broadcasts\n");
}

void test_send_beacon_event_counter_monotonic() {
  /* Continues from the prior test's state — counter started at 1 and
   * the AA:...:01 peer is in the table. Don't add more peers; one peer
   * is enough to verify the counter increments per send (each broadcast
   * iterates all peers, so adding peers would multiply the captured
   * frame count without changing the monotonicity contract). */
  g_outs.clear();

  /* Three sends. Counters should be 2, 3, 4 on the captured frames. */
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::DEPARTED,
                                         "office", 20000));
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                         "office", 21000));
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::DEPARTED,
                                         "office", 22000));
  assert(g_outs.size() == 3);

  /* counter is LE 64-bit; offset = session-prefix(1) + envelope
   * OFFSET_COUNTER. Use the canonical constant from mesh_envelope.h
   * rather than hand-rolled 1+1+16+8. */
  const size_t cnt_off = mesh_session::MSGTYPE_HEADER_LEN
                       + mesh_envelope::OFFSET_COUNTER;
  uint64_t prev = 1;   /* prior test left counter at 1 */
  for (const auto& f : g_outs) {
    uint64_t c = 0;
    for (size_t i = 0; i < mesh_envelope::COUNTER_LEN; ++i) {
      c |= ((uint64_t)f.bytes[cnt_off + i]) << (8 * i);
    }
    assert(c > prev);
    prev = c;
  }
  std::printf("PASS test_send_beacon_event_counter_monotonic\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * PR 5c-4 — receive-side dispatch
 * ──────────────────────────────────────────────────────────────────────── */

struct ReceivedEvent {
  uint8_t                   sender_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_beacon::BeaconState  state;
  char                      label[mesh_beacon::MAX_LABEL_BYTES + 1];
};
std::vector<ReceivedEvent> g_received;

void on_beacon_event_received(const uint8_t* sender_fp,
                              mesh_beacon::BeaconState state,
                              const char* label) {
  ReceivedEvent r;
  std::memcpy(r.sender_fp, sender_fp, sizeof(r.sender_fp));
  r.state = state;
  std::strncpy(r.label, label ? label : "", sizeof(r.label) - 1);
  r.label[sizeof(r.label) - 1] = '\0';
  g_received.push_back(r);
}

/* Helper: build a signed BEACON_EVENT session frame (1 + 38 + 25 + 64
 * = 128 bytes) for `sender_pub`/`sender_priv`. Returns the frame bytes
 * in `out_frame` (must be at least 128 bytes). */
size_t build_beacon_frame(const uint8_t sender_pub[mesh_crypto::PUBKEY_LEN],
                          const uint8_t sender_priv[mesh_crypto::PRIVKEY_LEN],
                          const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                          uint64_t counter,
                          mesh_beacon::BeaconState state,
                          const char* label,
                          uint8_t* out_frame, size_t out_cap) {
  /* Encode payload. */
  uint8_t payload[mesh_beacon::PAYLOAD_LEN];
  if (!mesh_beacon::encode(state, label, payload, sizeof(payload))) return 0;

  /* Build header. */
  mesh_envelope::Header h;
  h.version  = mesh_envelope::PROTOCOL_VERSION;
  h.msg_type = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  mesh_crypto::compute_opera_id(opera_secret, h.opera_id);
  mesh_crypto::compute_fingerprint(sender_pub, h.sender_fp);
  h.counter   = counter;
  h.timestamp = 12345;

  /* Serialize+sign. Out goes after the 1-byte session prefix. */
  if (out_cap < 1 + mesh_envelope::MAX_FRAME_LEN) return 0;
  out_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  const size_t n = mesh_envelope::serialize_signed(
      h, payload, sizeof(payload), sender_priv, sender_pub,
      out_frame + 1, out_cap - 1);
  if (n == 0) return 0;
  return 1 + n;
}

void test_register_trusted_peer_basic() {
  reset_world();
  assert(mesh_session::trusted_peer_count() == 0);

  uint8_t peer_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t peer_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(peer_pub, peer_priv));
  assert(mesh_session::register_trusted_peer(peer_pub));
  assert(mesh_session::trusted_peer_count() == 1);

  /* Dedup: re-registering the same pubkey returns false. */
  assert(!mesh_session::register_trusted_peer(peer_pub));
  assert(mesh_session::trusted_peer_count() == 1);

  mesh_session::clear_trusted_peers();
  assert(mesh_session::trusted_peer_count() == 0);
  std::printf("PASS test_register_trusted_peer_basic\n");
}

void test_beacon_event_roundtrip() {
  /* Stand up the receiver session with its own keypair + opera_secret. */
  reset_world();
  mesh_session::deinit();

  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t rx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(rx_pub, rx_priv));
  assert(mesh_session::init(rx_pub, rx_priv));
  assert(mesh_session::start());

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xE0 + i);
  assert(mesh_session::set_opera_secret(opera_secret));

  /* Sender keypair — register as trusted on the receiver side. */
  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));

  g_received.clear();
  mesh_session::set_beacon_event_handler(on_beacon_event_received);

  /* Build + inject a signed BEACON_EVENT frame from the sender. */
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/7,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      frame, sizeof(frame));
  assert(flen > 0);

  uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  /* mesh_transport's drain_ring only forwards frames whose source MAC
   * is a known peer; add_peer registers it so the dispatch fires. */
  assert(mesh_transport::add_peer(mac));
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();   /* drains the recv ring → on_transport_recv */

  assert(g_received.size() == 1);
  assert(g_received[0].state == mesh_beacon::BeaconState::ARRIVED);
  assert(std::strcmp(g_received[0].label, "kitchen") == 0);
  /* sender_fp matches compute_fingerprint(tx_pub). */
  uint8_t expected_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, expected_fp);
  assert(std::memcmp(g_received[0].sender_fp, expected_fp, sizeof(expected_fp)) == 0);
  std::printf("PASS test_beacon_event_roundtrip\n");
}

void test_beacon_event_replay_dropped() {
  /* Continues from the prior test's state — same receiver, same sender,
   * but inject the SAME frame (counter=7) twice. The replay must be
   * dropped silently. */
  g_received.clear();

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  mesh_session::clear_trusted_peers();   /* fresh start */
  assert(mesh_session::register_trusted_peer(tx_pub));

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xE0 + i);

  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "replay",
      frame, sizeof(frame));
  assert(flen > 0);

  uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  assert(mesh_transport::add_peer(mac));   /* required by drain_ring */
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  /* Inject the IDENTICAL frame again. */
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();

  /* Only one event should have been delivered. */
  assert(g_received.size() == 1);

  /* A NEWER counter from the same peer DOES pass through. */
  uint8_t frame2[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t flen2 = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/2,
      mesh_beacon::BeaconState::DEPARTED, "replay",
      frame2, sizeof(frame2));
  assert(flen2 > 0);
  mesh_transport::test::inject_recv(mac, frame2, flen2, -55);
  mesh_transport::process();
  assert(g_received.size() == 2);
  assert(g_received[1].state == mesh_beacon::BeaconState::DEPARTED);

  std::printf("PASS test_beacon_event_replay_dropped\n");
}

void test_beacon_event_unknown_sender_dropped() {
  reset_world();
  mesh_session::deinit();

  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t rx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(rx_pub, rx_priv));
  assert(mesh_session::init(rx_pub, rx_priv));
  assert(mesh_session::start());

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xF0 + i);
  assert(mesh_session::set_opera_secret(opera_secret));

  /* DO NOT register the sender. */
  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));

  g_received.clear();
  mesh_session::set_beacon_event_handler(on_beacon_event_received);

  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "intruder",
      frame, sizeof(frame));
  assert(flen > 0);

  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  assert(mesh_transport::add_peer(mac));
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  assert(g_received.empty());
  std::printf("PASS test_beacon_event_unknown_sender_dropped\n");
}

void test_beacon_event_forged_signature_dropped() {
  reset_world();
  mesh_session::deinit();

  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t rx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(rx_pub, rx_priv));
  assert(mesh_session::init(rx_pub, rx_priv));
  assert(mesh_session::start());

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xC0 + i);
  assert(mesh_session::set_opera_secret(opera_secret));

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));

  g_received.clear();
  mesh_session::set_beacon_event_handler(on_beacon_event_received);

  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "tamper",
      frame, sizeof(frame));
  assert(flen > 0);

  /* Flip the first payload byte, located at session-prefix +
   * mesh_envelope::OFFSET_PAYLOAD (the canonical header-layout
   * constant — see mesh_envelope.h). The flip invalidates the
   * signature but leaves the sender_fp peek successful, so the
   * parse_and_verify step is what drops the frame. */
  frame[mesh_session::MSGTYPE_HEADER_LEN + mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;

  uint8_t mac[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
  assert(mesh_transport::add_peer(mac));
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  assert(g_received.empty());
  std::printf("PASS test_beacon_event_forged_signature_dropped\n");
}

void test_peer_link_mac_binding() {
  /* get_peer_links: the MAC↔fingerprint binding is learned ONLY from a
   * fully verified frame — never from an unverified one — and refreshes
   * when the peer speaks from a new address. */
  reset_world();
  mesh_session::deinit();

  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t rx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(rx_pub, rx_priv));
  assert(mesh_session::init(rx_pub, rx_priv));
  assert(mesh_session::start());

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xA0 + i);
  assert(mesh_session::set_opera_secret(opera_secret));

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));

  uint8_t expected_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, expected_fp);

  /* Before any frame: the entry is listed but its MAC is unknown. */
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(std::memcmp(links[0].fp, expected_fp, sizeof(expected_fp)) == 0);
  assert(!links[0].mac_known);

  /* A frame whose signature does NOT verify must not bind a MAC —
   * otherwise anyone on the channel could relabel a peer's liveness. */
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "forged",
      frame, sizeof(frame));
  assert(flen > 0);
  frame[mesh_session::MSGTYPE_HEADER_LEN + mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  uint8_t mac_forged[6] = {0xDE, 0xAD, 0xDE, 0xAD, 0xDE, 0xAD};
  assert(mesh_transport::add_peer(mac_forged));
  mesh_transport::test::inject_recv(mac_forged, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);

  /* A verified frame binds its source MAC. */
  uint8_t mac_a[6] = {0x02, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
  flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/2,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      frame, sizeof(frame));
  assert(flen > 0);
  assert(mesh_transport::add_peer(mac_a));
  mesh_transport::test::inject_recv(mac_a, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].mac_known);
  assert(std::memcmp(links[0].mac, mac_a, sizeof(mac_a)) == 0);

  /* A REPLAYED frame from a different MAC must not rebind — the
   * counter check drops it before the MAC is recorded. */
  mesh_transport::test::inject_recv(mac_forged, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(std::memcmp(links[0].mac, mac_a, sizeof(mac_a)) == 0);

  /* The peer reboots onto a new address: the next verified frame
   * refreshes the binding. */
  uint8_t mac_b[6] = {0x02, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E};
  flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/3,
      mesh_beacon::BeaconState::DEPARTED, "kitchen",
      frame, sizeof(frame));
  assert(flen > 0);
  assert(mesh_transport::add_peer(mac_b));
  mesh_transport::test::inject_recv(mac_b, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].mac_known);
  assert(std::memcmp(links[0].mac, mac_b, sizeof(mac_b)) == 0);

  /* clear_trusted_peers wipes the binding with the table. */
  mesh_session::clear_trusted_peers();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 0);

  std::printf("PASS test_peer_link_mac_binding\n");
}

void test_deinit_clears_opera_auth_state() {
  /* Regression for the codex P1 missed at PR #472 merge time: deinit()
   * did not clear the opera-auth bookkeeping, so a deinit()/init()
   * cycle would leave has_opera_secret() returning true from the
   * previous run, and the next send_beacon_event() would sign with
   * a stale opera_id / fingerprint and a continuing counter. */
  reset_world();

  /* Stand up a fresh session and set an opera secret. */
  mesh_session::deinit();
  uint8_t pub[mesh_crypto::PUBKEY_LEN];
  uint8_t priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::init(pub, priv));
  assert(mesh_session::start());

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0xA0 + i);
  assert(mesh_session::set_opera_secret(opera_secret));
  assert(mesh_session::has_opera_secret());

  /* Tear down, then re-init with a different keypair. has_opera_secret
   * must report false until set_opera_secret() is called again. */
  mesh_session::deinit();
  assert(!mesh_session::has_opera_secret());

  uint8_t pub2[mesh_crypto::PUBKEY_LEN];
  uint8_t priv2[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub2, priv2));
  assert(mesh_session::init(pub2, priv2));
  assert(mesh_session::start());
  assert(!mesh_session::has_opera_secret());

  /* send_beacon_event must refuse before the new set_opera_secret(). */
  uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x05};
  assert(mesh_transport::add_peer(peer_mac));
  g_outs.clear();
  assert(!mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                          "stale", 1000));
  assert(g_outs.empty());

  /* After set_opera_secret() with the SAME secret but a different
   * keypair, the counter restarts at 0 and the next send produces
   * counter=1 (not a continuation from the prior session). */
  assert(mesh_session::set_opera_secret(opera_secret));
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                         "fresh", 2000));
  assert(g_outs.size() == 1);
  /* counter at session-prefix + envelope OFFSET_COUNTER, LE 64-bit. */
  const size_t cnt_off = mesh_session::MSGTYPE_HEADER_LEN
                       + mesh_envelope::OFFSET_COUNTER;
  uint64_t c = 0;
  for (size_t i = 0; i < mesh_envelope::COUNTER_LEN; ++i) {
    c |= ((uint64_t)g_outs[0].bytes[cnt_off + i]) << (8 * i);
  }
  assert(c == 1);
  std::printf("PASS test_deinit_clears_opera_auth_state\n");
}

void test_get_paired_peer_pubkey_gated_by_state() {
  /* Before pairing reaches AWAITING_CONFIRM, the peer_pubkey field
   * in PairingContext is zero-initialized — exposing it would let
   * the integration layer act on stale (or absent) data. The
   * accessor returns false in DISCOVERING / OFFERED / etc. states
   * and only succeeds once OFFER/ACCEPT has populated peer_pubkey. */
  reset_world();

  uint8_t buf[mesh_crypto::PUBKEY_LEN];
  std::memset(buf, 0xCC, sizeof(buf));

  /* DISCOVERING_INITIATOR — no peer pubkey captured yet. */
  assert(mesh_session::start_pairing_initiator(
      /*opera_secret=*/(const uint8_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
      "test-opera", /*now_ms=*/1000));
  assert(!mesh_session::get_paired_peer_pubkey(buf));
  /* Poison must be intact — accessor MUST NOT touch out on failure. */
  for (size_t i = 0; i < sizeof(buf); ++i) assert(buf[i] == 0xCC);

  /* nullptr arg also returns false. */
  assert(!mesh_session::get_paired_peer_pubkey(nullptr));

  std::printf("PASS test_get_paired_peer_pubkey_gated_by_state\n");
}

/* ── PR-8: status accessors + mesh REST API JSON builders ─────────────── */

void test_get_opera_id_matches_compute() {
  /* get_opera_id() must hand back exactly the opera_id mesh_crypto
   * derives from the secret (same value set_opera_secret cached). */
  reset_world();

  uint8_t out[mesh_crypto::OPERA_ID_LEN];
  /* Before any secret is set, get_opera_id returns false and leaves out
   * untouched. */
  std::memset(out, 0x7E, sizeof(out));
  assert(!mesh_session::has_opera());
  assert(!mesh_session::get_opera_id(out));
  for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == 0x7E);

  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x10 + i);
  assert(mesh_session::set_opera_secret(secret));
  assert(mesh_session::has_opera());

  assert(mesh_session::get_opera_id(out));
  uint8_t expect[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(secret, expect);
  assert(std::memcmp(out, expect, sizeof(out)) == 0);
  /* nullptr is rejected. */
  assert(!mesh_session::get_opera_id(nullptr));
  std::printf("PASS test_get_opera_id_matches_compute\n");
}

void test_set_get_opera_name_ram_only() {
  reset_world();
  char buf[mesh_pairing::MAX_OPERA_NAME_LEN + 1];

  /* Fresh session: empty name, always null-terminated. */
  std::memset(buf, 0x5A, sizeof(buf));
  mesh_session::get_opera_name(buf, sizeof(buf));
  assert(buf[0] == '\0');

  mesh_session::set_opera_name("Living Room Opera");
  mesh_session::get_opera_name(buf, sizeof(buf));
  assert(std::strcmp(buf, "Living Room Opera") == 0);

  /* Over-long name is truncated to capacity, still null-terminated. */
  char longname[mesh_pairing::MAX_OPERA_NAME_LEN + 16];
  std::memset(longname, 'A', sizeof(longname) - 1);
  longname[sizeof(longname) - 1] = '\0';
  mesh_session::set_opera_name(longname);
  mesh_session::get_opera_name(buf, sizeof(buf));
  assert(std::strlen(buf) == mesh_pairing::MAX_OPERA_NAME_LEN);

  /* nullptr clears. */
  mesh_session::set_opera_name(nullptr);
  mesh_session::get_opera_name(buf, sizeof(buf));
  assert(buf[0] == '\0');
  std::printf("PASS test_set_get_opera_name_ram_only\n");
}

void test_mesh_state_name_mapping() {
  using S = mesh_pairing::State;
  /* Pairing precedence wins over steady-state classification. */
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::DISCOVERING_INITIATOR, 5), "PAIRING_INIT") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::AWAITING_ACCEPT, 5), "PAIRING_INIT") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, false, S::DISCOVERING_JOINER, 0), "PAIRING_JOIN") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, false, S::AWAITING_COMPLETE, 0), "PAIRING_JOIN") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::AWAITING_CONFIRM, 0), "PAIRING_CONFIRM") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::AWAITING_CONFIRM_PEER, 0), "PAIRING_CONFIRM") == 0);

  /* Steady states (IDLE / PAIRED / FAILED fall through). */
  assert(std::strcmp(mesh_pairing::mesh_state_name(false, true, S::IDLE, 3), "DISABLED") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, false, S::IDLE, 0), "NO_OPERA") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::IDLE, 0), "CONNECTING") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::IDLE, 2), "ACTIVE") == 0);
  assert(std::strcmp(mesh_pairing::mesh_state_name(true, true, S::PAIRED, 1), "ACTIVE") == 0);
  std::printf("PASS test_mesh_state_name_mapping\n");
}

void test_build_mesh_status_json_active() {
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  for (size_t i = 0; i < sizeof(opera_id); ++i) opera_id[i] = (uint8_t)(0xA0 + i);

  char buf[512];
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), /*enabled=*/true, /*has_opera=*/true,
      opera_id, "Home", mesh_pairing::State::IDLE,
      /*peers_total=*/3, /*peers_online=*/2, /*alerts=*/7, /*code=*/123456));

  /* Field presence + the exact strings the UI reads. */
  assert(std::strstr(buf, "\"ok\":true") != nullptr);
  assert(std::strstr(buf, "\"state\":\"ACTIVE\"") != nullptr);
  assert(std::strstr(buf, "\"opera_id\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"") != nullptr);
  assert(std::strstr(buf, "\"opera_name\":\"Home\"") != nullptr);
  assert(std::strstr(buf, "\"has_opera\":true") != nullptr);
  assert(std::strstr(buf, "\"enabled\":true") != nullptr);
  assert(std::strstr(buf, "\"peers_total\":3") != nullptr);
  assert(std::strstr(buf, "\"peers_online\":2") != nullptr);
  assert(std::strstr(buf, "\"alerts_received\":7") != nullptr);
  /* No pairing_code leak outside PAIRING_CONFIRM. */
  assert(std::strstr(buf, "pairing_code") == nullptr);
  std::printf("PASS test_build_mesh_status_json_active\n");
}

void test_build_mesh_status_json_pairing_code_only_in_confirm() {
  char buf[512];
  /* PAIRING_CONFIRM → code present. */
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, true, nullptr, "X",
      mesh_pairing::State::AWAITING_CONFIRM, 1, 0, 0, /*code=*/42));
  assert(std::strstr(buf, "\"state\":\"PAIRING_CONFIRM\"") != nullptr);
  assert(std::strstr(buf, "\"pairing_code\":42") != nullptr);

  /* PAIRING_INIT → NO code (early-leak guard). */
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, true, nullptr, "X",
      mesh_pairing::State::DISCOVERING_INITIATOR, 1, 0, 0, /*code=*/42));
  assert(std::strstr(buf, "\"state\":\"PAIRING_INIT\"") != nullptr);
  assert(std::strstr(buf, "pairing_code") == nullptr);
  std::printf("PASS test_build_mesh_status_json_pairing_code_only_in_confirm\n");
}

void test_build_mesh_status_json_escapes_name() {
  char buf[512];
  /* A name with a quote must not break the JSON envelope. */
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, false, nullptr, "Evil\"name",
      mesh_pairing::State::IDLE, 0, 0, 0, 0));
  assert(std::strstr(buf, "\"opera_name\":\"Evil\\\"name\"") != nullptr);
  std::printf("PASS test_build_mesh_status_json_escapes_name\n");
}

void test_build_mesh_status_json_no_opera_empty_id() {
  char buf[512];
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, false, nullptr, "",
      mesh_pairing::State::IDLE, 0, 0, 0, 0));
  assert(std::strstr(buf, "\"state\":\"NO_OPERA\"") != nullptr);
  assert(std::strstr(buf, "\"opera_id\":\"\"") != nullptr);
  assert(std::strstr(buf, "\"has_opera\":false") != nullptr);
  std::printf("PASS test_build_mesh_status_json_no_opera_empty_id\n");
}

void test_build_mesh_peers_json() {
  mesh_api::PeerView views[2];
  std::strcpy(views[0].fingerprint, "0011223344556677");
  std::strcpy(views[0].name, "Kitchen");
  views[0].state = "CONNECTED";
  views[0].last_seen_sec = 12;
  views[0].rssi = -42;
  views[0].alerts_received = 3;
  std::strcpy(views[1].fingerprint, "8899aabbccddeeff");
  views[1].name[0] = '\0';
  views[1].state = "OFFLINE";
  views[1].last_seen_sec = 0xFFFFFFFFu;
  views[1].rssi = 0;
  views[1].alerts_received = 0;

  char buf[1024];
  assert(mesh_api::build_mesh_peers_json(buf, sizeof(buf), views, 2));
  assert(std::strstr(buf, "\"ok\":true") != nullptr);
  assert(std::strstr(buf, "\"fingerprint\":\"0011223344556677\"") != nullptr);
  assert(std::strstr(buf, "\"name\":\"Kitchen\"") != nullptr);
  assert(std::strstr(buf, "\"state\":\"CONNECTED\"") != nullptr);
  assert(std::strstr(buf, "\"rssi\":-42") != nullptr);
  assert(std::strstr(buf, "\"fingerprint\":\"8899aabbccddeeff\"") != nullptr);
  assert(std::strstr(buf, "\"name\":\"\"") != nullptr);
  /* Spec §8.2 per-peer alert count (F11 residual). */
  assert(std::strstr(buf, "\"rssi\":-42,\"alerts_received\":3}") != nullptr);
  assert(std::strstr(buf, "\"rssi\":0,\"alerts_received\":0}") != nullptr);

  /* Empty peer list still yields a valid envelope. */
  assert(mesh_api::build_mesh_peers_json(buf, sizeof(buf), nullptr, 0));
  assert(std::strcmp(buf, "{\"ok\":true,\"peers\":[]}") == 0);
  std::printf("PASS test_build_mesh_peers_json\n");
}

void test_build_mesh_json_buffer_too_small() {
  char tiny[8];
  /* status + peers both must fail cleanly (false) on overflow, not
   * scribble past the buffer. */
  assert(!mesh_api::build_mesh_status_json(
      tiny, sizeof(tiny), true, true, nullptr, "name",
      mesh_pairing::State::IDLE, 0, 0, 0, 0));
  assert(!mesh_api::build_mesh_peers_json(tiny, sizeof(tiny), nullptr, 0));
  std::printf("PASS test_build_mesh_json_buffer_too_small\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * F10 — enable/disable, leave, the alerts channel (+ F11 attribution)
 * ──────────────────────────────────────────────────────────────────────── */

struct ReceivedAlert {
  uint8_t          fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_alert::Kind kind;
  uint8_t          severity;
  uint32_t         witness_seq;
};
std::vector<ReceivedAlert> g_alerts_rx;

void on_alert_rx(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                 mesh_alert::Kind kind, uint8_t severity, uint32_t seq) {
  ReceivedAlert r;
  std::memcpy(r.fp, fp, sizeof(r.fp));
  r.kind = kind;
  r.severity = severity;
  r.witness_seq = seq;
  g_alerts_rx.push_back(r);
}

struct LeftPeer {
  uint8_t fp [mesh_crypto::FINGERPRINT_LEN];
  uint8_t pub[mesh_crypto::PUBKEY_LEN];
  size_t  trusted_at_callback;
};
std::vector<LeftPeer> g_left;

void on_peer_left(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                  const uint8_t pub[mesh_crypto::PUBKEY_LEN]) {
  LeftPeer l;
  std::memcpy(l.fp, fp, sizeof(l.fp));
  std::memcpy(l.pub, pub, sizeof(l.pub));
  l.trusted_at_callback = mesh_session::trusted_peer_count();
  g_left.push_back(l);
}

/* Generic sender-side builder: [session msg-type][signed envelope] for
 * any opera-authenticated msg_type + payload, signed by sender_*. The
 * TAMPER_ALERT / LEAVE_OPERA twin of build_beacon_frame. */
size_t build_signed_session_frame(const uint8_t sender_pub[mesh_crypto::PUBKEY_LEN],
                                  const uint8_t sender_priv[mesh_crypto::PRIVKEY_LEN],
                                  const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                                  uint64_t counter,
                                  mesh_envelope::MsgType type,
                                  const uint8_t* payload, size_t payload_len,
                                  uint8_t* out_frame, size_t out_cap) {
  mesh_envelope::Header h;
  h.version  = mesh_envelope::PROTOCOL_VERSION;
  h.msg_type = static_cast<uint8_t>(type);
  mesh_crypto::compute_opera_id(opera_secret, h.opera_id);
  mesh_crypto::compute_fingerprint(sender_pub, h.sender_fp);
  h.counter   = counter;
  h.timestamp = 12345;
  if (out_cap < 1 + mesh_envelope::MAX_FRAME_LEN) return 0;
  out_frame[0] = static_cast<uint8_t>(type);
  const size_t n = mesh_envelope::serialize_signed(
      h, payload, payload_len, sender_priv, sender_pub,
      out_frame + 1, out_cap - 1);
  return n == 0 ? 0 : 1 + n;
}

size_t build_alert_frame(const uint8_t pub[mesh_crypto::PUBKEY_LEN],
                         const uint8_t priv[mesh_crypto::PRIVKEY_LEN],
                         const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN],
                         uint64_t counter, mesh_alert::Kind kind,
                         uint8_t severity, uint32_t seq,
                         uint8_t* out, size_t cap) {
  uint8_t payload[mesh_alert::PAYLOAD_LEN];
  if (!mesh_alert::encode(kind, severity, seq, payload, sizeof(payload))) return 0;
  return build_signed_session_frame(pub, priv, secret, counter,
                                    mesh_envelope::MsgType::TAMPER_ALERT,
                                    payload, sizeof(payload), out, cap);
}

/* Fresh receiver session with its own device keypair (returned) and the
 * given opera secret already set. */
void stand_up_session(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN],
                      uint8_t pub[mesh_crypto::PUBKEY_LEN],
                      uint8_t priv[mesh_crypto::PRIVKEY_LEN]) {
  reset_world();
  mesh_session::deinit();
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::init(pub, priv));
  mesh_session::set_paired_callback(on_paired);
  mesh_session::set_failed_callback(on_failed);
  mesh_session::set_code_ready_callback(on_code_ready);
  assert(mesh_session::start());
  if (secret != nullptr) assert(mesh_session::set_opera_secret(secret));
  g_alerts_rx.clear();
  g_left.clear();
}

void inject_from(const uint8_t mac[6], const uint8_t* frame, size_t len) {
  mesh_transport::add_peer(mac);   /* drain_ring forwards known MACs only */
  mesh_transport::test::inject_recv(mac, frame, len, -50);
  mesh_transport::process();
}

void test_tamper_alert_roundtrip() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x31 + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN], tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  mesh_session::set_tamper_alert_handler(on_alert_rx);

  /* The receive path stamps records with the latest process() clock. */
  mesh_session::process(4242);

  const uint8_t mac[6] = {0x02, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_alert_frame(tx_pub, tx_priv, secret, /*counter=*/5,
                                  mesh_alert::Kind::CAMERA_TAMPER, 6, 777,
                                  frame, sizeof(frame));
  assert(flen == 1 + mesh_envelope::HEADER_LEN + mesh_alert::PAYLOAD_LEN
                   + mesh_envelope::SIGNATURE_LEN);
  inject_from(mac, frame, flen);

  /* Handler fired with the sender's fingerprint + decoded payload. */
  assert(g_alerts_rx.size() == 1);
  assert(std::memcmp(g_alerts_rx[0].fp, tx_fp, sizeof(tx_fp)) == 0);
  assert(g_alerts_rx[0].kind == mesh_alert::Kind::CAMERA_TAMPER);
  assert(g_alerts_rx[0].severity == 6);
  assert(g_alerts_rx[0].witness_seq == 777);

  /* F11: opera-wide AND per-peer attribution. */
  assert(mesh_session::alerts_received() == 1);
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].alerts_received == 1);

  /* History ring holds the record. */
  mesh_alert::Record recs[mesh_session::MAX_ALERT_HISTORY];
  assert(mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 1);
  assert(recs[0].timestamp_ms == 4242);
  assert(std::memcmp(recs[0].sender_fp, tx_fp, sizeof(tx_fp)) == 0);
  assert(recs[0].kind == mesh_alert::Kind::CAMERA_TAMPER);
  assert(recs[0].severity == 6);
  assert(recs[0].witness_seq == 777);

  /* A replay (same counter) does not double-count. */
  inject_from(mac, frame, flen);
  assert(g_alerts_rx.size() == 1);
  assert(mesh_session::alerts_received() == 1);

  /* A forgery (payload bit flipped under a fresh counter) counts nothing. */
  flen = build_alert_frame(tx_pub, tx_priv, secret, 6,
                           mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 778,
                           frame, sizeof(frame));
  frame[mesh_session::MSGTYPE_HEADER_LEN + mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  inject_from(mac, frame, flen);
  assert(mesh_session::alerts_received() == 1);

  /* A correctly-signed but malformed payload (7 bytes) counts nothing. */
  uint8_t bad_payload[mesh_alert::PAYLOAD_LEN + 1] = {0, 6, 1, 0, 0, 0, 0};
  flen = build_signed_session_frame(tx_pub, tx_priv, secret, 7,
                                    mesh_envelope::MsgType::TAMPER_ALERT,
                                    bad_payload, sizeof(bad_payload),
                                    frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(mesh_session::alerts_received() == 1);
  assert(g_alerts_rx.size() == 1);

  /* A second genuine alert: counters 2, history newest-first. */
  mesh_session::process(5000);
  flen = build_alert_frame(tx_pub, tx_priv, secret, 8,
                           mesh_alert::Kind::TEMP_DRIFT, 3, 0,
                           frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(mesh_session::alerts_received() == 2);
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].alerts_received == 2);
  assert(mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 2);
  assert(recs[0].kind == mesh_alert::Kind::TEMP_DRIFT && recs[0].timestamp_ms == 5000);
  assert(recs[1].kind == mesh_alert::Kind::CAMERA_TAMPER);
  /* cap is honored. */
  assert(mesh_session::get_alerts(recs, 1) == 1);
  assert(recs[0].kind == mesh_alert::Kind::TEMP_DRIFT);

  /* DELETE semantics: history empties, the lifetime counters stay. */
  mesh_session::clear_alerts();
  assert(mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 0);
  assert(mesh_session::alerts_received() == 2);
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].alerts_received == 2);

  /* deinit wipes counters and history. */
  mesh_session::deinit();
  assert(mesh_session::alerts_received() == 0);
  assert(mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 0);
  std::printf("PASS test_tamper_alert_roundtrip\n");
}

void test_alert_ring_wraps_newest_first() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x71 + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN], tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));

  const uint8_t mac[6] = {0x02, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
  const size_t total = mesh_session::MAX_ALERT_HISTORY + 3;
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  for (size_t i = 1; i <= total; ++i) {
    const size_t flen = build_alert_frame(tx_pub, tx_priv, secret, i,
                                          mesh_alert::Kind::ENCLOSURE_TAMPER, 6,
                                          (uint32_t)(1000 + i), frame, sizeof(frame));
    inject_from(mac, frame, flen);
  }
  assert(mesh_session::alerts_received() == total);
  mesh_alert::Record recs[mesh_session::MAX_ALERT_HISTORY + 4];
  const size_t n = mesh_session::get_alerts(recs, sizeof(recs) / sizeof(recs[0]));
  assert(n == mesh_session::MAX_ALERT_HISTORY);
  for (size_t k = 0; k < n; ++k) {
    assert(recs[k].witness_seq == (uint32_t)(1000 + total - k));
  }
  std::printf("PASS test_alert_ring_wraps_newest_first\n");
}

void test_send_tamper_alert() {
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(nullptr, pub, priv);
  const uint8_t peer_mac[6] = {0x02, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5};
  assert(mesh_transport::add_peer(peer_mac));

  /* No opera yet → refused, nothing on the air. */
  g_outs.clear();
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1, 10));
  assert(g_outs.empty());

  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x91 + i);
  assert(mesh_session::set_opera_secret(secret));

  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 4321, 20));
  assert(g_outs.size() == 1);
  const std::vector<uint8_t>& f = g_outs[0].bytes;
  assert(f[0] == static_cast<uint8_t>(mesh_envelope::MsgType::TAMPER_ALERT));
  mesh_envelope::Header hdr;
  const uint8_t* payload = nullptr;
  size_t plen = 0;
  assert(mesh_envelope::parse_and_verify(f.data() + 1, f.size() - 1, pub,
                                         &hdr, &payload, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::TAMPER_ALERT));
  assert(hdr.counter == 1);
  uint8_t expect_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(secret, expect_id);
  assert(std::memcmp(hdr.opera_id, expect_id, sizeof(expect_id)) == 0);
  mesh_alert::Kind k;
  uint8_t sev = 0;
  uint32_t seq = 0;
  assert(mesh_alert::decode(payload, plen, &k, &sev, &seq));
  assert(k == mesh_alert::Kind::TEMP_DRIFT && sev == 3 && seq == 4321);

  /* Invalid severity → refused before anything is signed. */
  g_outs.clear();
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 8, 1, 30));
  assert(g_outs.empty());

  /* Disabled → refused; re-enabled → sends again. */
  mesh_session::set_enabled(false);
  assert(!mesh_session::is_enabled());
  assert(!mesh_session::is_running());
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 1, 40));
  assert(g_outs.empty());
  mesh_session::set_enabled(true);
  assert(mesh_session::is_enabled());
  assert(mesh_session::is_running());
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 1, 50));
  assert(g_outs.size() == 1);
  std::printf("PASS test_send_tamper_alert\n");
}

void test_enable_disable() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x51 + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN], tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  assert(mesh_session::is_enabled());

  /* Disabling while a pairing runs tears the pairing down. */
  g_failed_fired = false;
  assert(mesh_session::start_pairing_joiner(100));
  mesh_session::set_enabled(false);
  assert(g_failed_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  assert(!mesh_session::is_running());
  /* "disabled ⇒ not running": start() refuses, pairing refuses. */
  assert(!mesh_session::start());
  assert(!mesh_session::is_running());
  assert(!mesh_session::start_pairing_joiner(200));

  /* Inbound verified frames are not dispatched while disabled. */
  const uint8_t mac[6] = {0x02, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5};
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_alert_frame(tx_pub, tx_priv, secret, 1,
                                  mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1,
                                  frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_alerts_rx.empty());
  assert(mesh_session::alerts_received() == 0);

  /* GET /api/mesh reports DISABLED (membership is kept — not a leave). */
  assert(mesh_session::has_opera());
  char buf[512];
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), mesh_session::is_enabled(), mesh_session::has_opera(),
      nullptr, "", mesh_session::pairing_state(), 1, 1, 0, 0));
  assert(std::strstr(buf, "\"state\":\"DISABLED\"") != nullptr);
  assert(std::strstr(buf, "\"enabled\":false") != nullptr);

  /* Re-enabled: the same peer's next frame is dispatched again. */
  mesh_session::set_enabled(true);
  assert(mesh_session::is_running());
  flen = build_alert_frame(tx_pub, tx_priv, secret, 2,
                           mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 2,
                           frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_alerts_rx.size() == 1);
  assert(mesh_session::alerts_received() == 1);

  /* deinit restores the default (enabled). */
  mesh_session::set_enabled(false);
  mesh_session::deinit();
  assert(mesh_session::is_enabled());
  std::printf("PASS test_enable_disable\n");
}

void test_leave_opera() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x13 + i);
  /* This device = A. */
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, a_pub, a_priv);
  mesh_session::set_opera_name("Home");
  uint8_t b_pub[mesh_crypto::PUBKEY_LEN], b_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  const uint8_t b_mac[6] = {0x02, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5};
  assert(mesh_transport::add_peer(b_mac));

  g_outs.clear();
  assert(mesh_session::leave_opera(5000));
  /* Exactly one frame: LEAVE_OPERA, empty payload, signed by A under the
   * opera_id it is leaving. */
  assert(g_outs.size() == 1);
  const std::vector<uint8_t> leave = g_outs[0].bytes;
  assert(leave[0] == static_cast<uint8_t>(mesh_envelope::MsgType::LEAVE_OPERA));
  assert(leave[0] == 25);
  assert(leave.size() == 1 + mesh_envelope::MIN_FRAME_LEN);
  mesh_envelope::Header hdr;
  const uint8_t* payload = nullptr;
  size_t plen = 99;
  assert(mesh_envelope::parse_and_verify(leave.data() + 1, leave.size() - 1, a_pub,
                                         &hdr, &payload, &plen));
  assert(plen == 0);
  uint8_t expect_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(secret, expect_id);
  assert(std::memcmp(hdr.opera_id, expect_id, sizeof(expect_id)) == 0);

  /* A has forgotten everything opera-scoped. */
  assert(!mesh_session::has_opera());
  uint8_t id[mesh_crypto::OPERA_ID_LEN];
  assert(!mesh_session::get_opera_id(id));
  assert(mesh_session::trusted_peer_count() == 0);
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(name, sizeof(name));
  assert(name[0] == '\0');
  assert(mesh_session::alerts_received() == 0);
  g_outs.clear();
  assert(!mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "x", 6000));
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1, 6000));
  assert(g_outs.empty());

  /* Leaving again with no opera: local no-op, nothing sent. */
  assert(!mesh_session::leave_opera(7000));
  assert(g_outs.empty());

  /* Second context: B, still in the opera, verifies A's LEAVE and drops
   * exactly A. */
  mesh_session::deinit();
  assert(mesh_session::init(b_pub, b_priv));
  assert(mesh_session::start());
  assert(mesh_session::set_opera_secret(secret));
  assert(mesh_session::register_trusted_peer(a_pub));
  g_left.clear();
  mesh_session::set_peer_left_handler(on_peer_left);
  const uint8_t a_mac[6] = {0x02, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5};
  inject_from(a_mac, leave.data(), leave.size());
  assert(g_left.size() == 1);
  uint8_t a_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  assert(std::memcmp(g_left[0].fp, a_fp, sizeof(a_fp)) == 0);
  assert(std::memcmp(g_left[0].pub, a_pub, sizeof(a_pub)) == 0);
  assert(mesh_session::trusted_peer_count() == 0);
  std::printf("PASS test_leave_opera\n");
}

void test_peer_left_dispatch() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x27 + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  mesh_session::set_peer_left_handler(on_peer_left);

  uint8_t x_pub[mesh_crypto::PUBKEY_LEN], x_priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t y_pub[mesh_crypto::PUBKEY_LEN], y_priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t z_pub[mesh_crypto::PUBKEY_LEN], z_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  assert(mesh_crypto::ed25519_generate_keypair(z_pub, z_priv));
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::register_trusted_peer(y_pub));
  uint8_t x_fp[mesh_crypto::FINGERPRINT_LEN], y_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  mesh_crypto::compute_fingerprint(y_pub, y_fp);

  const uint8_t mac[6] = {0x02, 0x11, 0x12, 0x13, 0x14, 0x15};
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];

  /* Forged LEAVE claiming to be X (signature broken): ignored. */
  size_t flen = build_signed_session_frame(x_pub, x_priv, secret, 1,
                                           mesh_envelope::MsgType::LEAVE_OPERA,
                                           nullptr, 0, frame, sizeof(frame));
  frame[flen - 1] ^= 0x01;
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);

  /* LEAVE from an unknown sender Z: ignored. */
  flen = build_signed_session_frame(z_pub, z_priv, secret, 1,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    nullptr, 0, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);

  /* LEAVE under a DIFFERENT opera (right key, wrong opera_id): ignored. */
  uint8_t other_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(other_secret); ++i) other_secret[i] = (uint8_t)(0xA7 + i);
  flen = build_signed_session_frame(x_pub, x_priv, other_secret, 2,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    nullptr, 0, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);

  /* A correctly-signed LEAVE carrying a payload is malformed: ignored. */
  const uint8_t junk[1] = {0x00};
  flen = build_signed_session_frame(x_pub, x_priv, secret, 3,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    junk, sizeof(junk), frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);

  /* A genuine LEAVE from X removes X — and only X — before the handler
   * runs, and hands the handler X's fingerprint + pubkey. */
  flen = build_signed_session_frame(x_pub, x_priv, secret, 4,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    nullptr, 0, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.size() == 1);
  assert(std::memcmp(g_left[0].fp, x_fp, sizeof(x_fp)) == 0);
  assert(std::memcmp(g_left[0].pub, x_pub, sizeof(x_pub)) == 0);
  assert(g_left[0].trusted_at_callback == 1);
  assert(mesh_session::trusted_peer_count() == 1);
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(std::memcmp(links[0].fp, y_fp, sizeof(y_fp)) == 0);

  /* Replaying it changes nothing (X is no longer a trusted sender). */
  inject_from(mac, frame, flen);
  assert(g_left.size() == 1);
  assert(mesh_session::trusted_peer_count() == 1);

  /* X can be re-registered (a re-pair) after leaving. */
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::trusted_peer_count() == 2);

  /* unregister_trusted_peer: by fingerprint, exactly once. */
  assert(mesh_session::unregister_trusted_peer(y_fp));
  assert(!mesh_session::unregister_trusted_peer(y_fp));
  assert(!mesh_session::unregister_trusted_peer(nullptr));
  assert(mesh_session::trusted_peer_count() == 1);
  std::printf("PASS test_peer_left_dispatch\n");
}

void test_build_mesh_alerts_json() {
  mesh_alert::Record recs[2];
  std::memset(recs, 0, sizeof(recs));
  recs[0].timestamp_ms = 90000;
  for (size_t i = 0; i < 8; ++i) recs[0].sender_fp[i] = (uint8_t)(0x10 + i);
  recs[0].kind = mesh_alert::Kind::CAMERA_TAMPER;
  recs[0].severity = 6;
  recs[0].witness_seq = 4242;
  recs[1].timestamp_ms = 1;
  recs[1].kind = static_cast<mesh_alert::Kind>(0x55);
  recs[1].severity = 3;

  char buf[1024];
  assert(mesh_api::build_mesh_alerts_json(buf, sizeof(buf), recs, 2));
  /* The exact field names the web UI's loadOperaAlerts() reads. */
  assert(std::strstr(buf, "{\"ok\":true,\"count\":2,\"alerts\":[{") == buf);
  assert(std::strstr(buf, "{\"timestamp_ms\":90000,\"type\":\"TAMPER\",\"severity\":6,"
                          "\"sender_fp\":\"1011121314151617\",\"sender_name\":\"\","
                          "\"detail\":\"camera_tamper\",\"witness_seq\":4242}") != nullptr);
  assert(std::strstr(buf, "\"detail\":\"unknown\"") != nullptr);

  /* Empty history is a valid envelope. */
  assert(mesh_api::build_mesh_alerts_json(buf, sizeof(buf), nullptr, 0));
  assert(std::strcmp(buf, "{\"ok\":true,\"count\":0,\"alerts\":[]}") == 0);

  /* Overflow fails cleanly; null records with count>0 refused. */
  char tiny[16];
  assert(!mesh_api::build_mesh_alerts_json(tiny, sizeof(tiny), recs, 2));
  assert(!mesh_api::build_mesh_alerts_json(buf, sizeof(buf), nullptr, 1));
  std::printf("PASS test_build_mesh_alerts_json\n");
}

void test_rest_buffers_fit_worst_case() {
  /* The handlers allocate exactly these caps; a full table of the widest
   * rows must fit (else the endpoint 500s with encode_failed). */
  static_assert(mesh_session::MAX_ALERT_HISTORY <= mesh_api::MAX_ALERTS_JSON,
                "alert cap pinned for MAX_ALERTS_JSON rows");
  static_assert(mesh_state::MAX_TRUSTED_PEERS == 8, "peer cap pinned for 8 rows");
  mesh_api::PeerView views[mesh_state::MAX_TRUSTED_PEERS];
  for (size_t i = 0; i < mesh_state::MAX_TRUSTED_PEERS; ++i) {
    std::strcpy(views[i].fingerprint, "ffffffffffffffff");
    views[i].name[0] = '\0';
    views[i].state = "CONNECTED";
    views[i].last_seen_sec = 0xFFFFFFFFu;
    views[i].rssi = -128;
    views[i].alerts_received = 0xFFFFFFFFu;
  }
  std::vector<char> buf(mesh_api::PEERS_JSON_CAP);
  assert(mesh_api::build_mesh_peers_json(buf.data(), buf.size(), views,
                                         mesh_state::MAX_TRUSTED_PEERS));
  /* ...and the pre-F11 1024-byte buffer would NOT have held it. */
  char old_buf[1024];
  assert(!mesh_api::build_mesh_peers_json(old_buf, sizeof(old_buf), views,
                                          mesh_state::MAX_TRUSTED_PEERS));

  mesh_alert::Record recs[mesh_api::MAX_ALERTS_JSON];
  for (size_t i = 0; i < mesh_api::MAX_ALERTS_JSON; ++i) {
    recs[i].timestamp_ms = 0xFFFFFFFFu;
    std::memset(recs[i].sender_fp, 0xFF, sizeof(recs[i].sender_fp));
    recs[i].kind = mesh_alert::Kind::ENCLOSURE_TAMPER;   /* longest name */
    recs[i].severity = 7;
    recs[i].witness_seq = 0xFFFFFFFFu;
  }
  std::vector<char> abuf(mesh_api::ALERTS_JSON_CAP);
  assert(mesh_api::build_mesh_alerts_json(abuf.data(), abuf.size(), recs,
                                          mesh_api::MAX_ALERTS_JSON));
  std::printf("PASS test_rest_buffers_fit_worst_case  (alerts=%zu B, peers=%zu B)\n",
              std::strlen(abuf.data()), std::strlen(buf.data()));
}

void test_build_mesh_status_json_disabled() {
  char buf[512];
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), /*enabled=*/false, /*has_opera=*/true, nullptr, "Home",
      mesh_pairing::State::IDLE, 2, 2, 5, 0));
  assert(std::strstr(buf, "\"state\":\"DISABLED\"") != nullptr);
  assert(std::strstr(buf, "\"enabled\":false") != nullptr);
  assert(std::strstr(buf, "\"alerts_received\":5") != nullptr);
  std::printf("PASS test_build_mesh_status_json_disabled\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * F10-rekey — remove_peer + opera_secret rotation through the session.
 * CRYPTO: maintainer review required before merge; bench-gated.
 * The session (a singleton) plays one side; a pure mesh_rekey::Context
 * held by the test plays the other, with the test signing its frames.
 * ──────────────────────────────────────────────────────────────────────── */

struct CommitRecord {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  std::vector<std::vector<uint8_t>> forgotten;
  bool persisted;
};
std::vector<CommitRecord> g_commits;

void on_rekey_commit(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN],
                     const uint8_t (*pks)[mesh_crypto::PUBKEY_LEN], size_t n) {
  CommitRecord c;
  std::memcpy(c.secret, secret, sizeof(c.secret));
  for (size_t i = 0; i < n; ++i) c.forgotten.emplace_back(pks[i], pks[i] + mesh_crypto::PUBKEY_LEN);
  /* The integration layer's re-persist through the FE gate (host stub). */
  c.persisted = mesh_state::persist_rotation(secret, pks, n);
  g_commits.push_back(c);
}

bool parse_session_frame(const std::vector<uint8_t>& f,
                         const uint8_t signer_pub[mesh_crypto::PUBKEY_LEN],
                         mesh_envelope::Header* hdr,
                         const uint8_t** payload, size_t* plen) {
  return f.size() > 1 &&
         mesh_envelope::parse_and_verify(f.data() + 1, f.size() - 1, signer_pub,
                                         hdr, payload, plen);
}

void test_rekey_session_as_initiator() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x61 + i);
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(S, a_pub, a_priv);          /* this device: A, the initiator */
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_commits.clear();

  uint8_t b_pub[32], b_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  uint8_t fp_a[8], fp_b[8], fp_x[8];
  mesh_crypto::compute_fingerprint(a_pub, fp_a);
  mesh_crypto::compute_fingerprint(b_pub, fp_b);
  mesh_crypto::compute_fingerprint(x_pub, fp_x);

  /* Both peers speak once so their MACs are bound. */
  const uint8_t mac_b[6] = {0x02, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B};
  const uint8_t mac_x[6] = {0x02, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C};
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_alert_frame(b_pub, b_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 0, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  flen = build_alert_frame(x_pub, x_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 0, frame, sizeof(frame));
  inject_from(mac_x, frame, flen);

  uint8_t old_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(old_id));

  g_outs.clear();
  uint8_t removed_pub[mesh_crypto::PUBKEY_LEN] = {0};
  assert(mesh_session::remove_peer(fp_x, 1000, removed_pub) ==
         mesh_session::RemoveResult::STARTED);
  assert(std::memcmp(removed_pub, x_pub, sizeof(x_pub)) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(mesh_session::rekey_in_progress());
  /* X's transport MAC is gone: the OFFER (and every later broadcast)
   * reaches B only. */
  assert(!mesh_transport::has_peer(mac_x));
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_b, 6) == 0);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_OFFER));
  assert(std::memcmp(hdr.opera_id, old_id, sizeof(old_id)) == 0);

  /* B's side of the exchange. */
  mesh_rekey::Context cb;
  mesh_rekey::context_init(cb);
  mesh_rekey::Action acc = mesh_rekey::receive(cb, fp_b, mesh_rekey::MsgType::OFFER,
                                               fp_a, pl, plen, 0);
  assert(acc.type == mesh_rekey::ActionType::SEND_ACCEPT);
  flen = build_signed_session_frame(b_pub, b_priv, S, 2, mesh_envelope::MsgType::REKEY_ACCEPT,
                                    acc.payload, acc.payload_len, frame, sizeof(frame));
  g_outs.clear();
  inject_from(mac_b, frame, flen);
  /* The session answers with B's SECRET, unicast to B's verified MAC. */
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_b, 6) == 0);
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_SECRET));
  mesh_rekey::Action inst = mesh_rekey::receive(cb, fp_b, mesh_rekey::MsgType::SECRET,
                                                fp_a, pl, plen, 0);
  assert(inst.type == mesh_rekey::ActionType::ACK_AND_INSTALL);
  assert(std::memcmp(inst.removed_fp, fp_x, 8) == 0);

  /* B ACKs under the OLD opera_id; the session commits. */
  assert(g_commits.empty());
  flen = build_signed_session_frame(b_pub, b_priv, S, 3, mesh_envelope::MsgType::REKEY_ACK,
                                    inst.payload, inst.payload_len, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  assert(g_commits.size() == 1);
  assert(std::memcmp(g_commits[0].secret, inst.new_secret, 32) == 0);
  assert(g_commits[0].forgotten.empty());
  assert(g_commits[0].persisted);
  assert(!mesh_session::rekey_in_progress());
  uint8_t new_id[mesh_crypto::OPERA_ID_LEN], expect_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(new_id));
  mesh_crypto::compute_opera_id(inst.new_secret, expect_id);
  assert(std::memcmp(new_id, expect_id, sizeof(new_id)) == 0);
  assert(std::memcmp(new_id, old_id, sizeof(new_id)) != 0);
  /* B is still trusted (re-registration is not needed: the table is keyed
   * by device key, not by opera); X is not. */
  assert(mesh_session::trusted_peer_count() == 1);
  assert(!mesh_session::unregister_trusted_peer(fp_x));

  /* After commit a frame under the OLD opera_id is rejected, one under
   * the NEW is accepted. */
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  g_alerts_rx.clear();
  flen = build_alert_frame(b_pub, b_priv, S, 4, mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  assert(g_alerts_rx.empty());
  flen = build_alert_frame(b_pub, b_priv, inst.new_secret, 5, mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 2, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  assert(g_alerts_rx.size() == 1);

  /* The outbound counter was NOT reset by the switch (receivers keep
   * per-fingerprint counters): OFFER=1, SECRET=2, so the next is 3. */
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 2000));
  assert(g_outs.size() == 1);
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.counter == 3);
  assert(std::memcmp(hdr.opera_id, new_id, sizeof(new_id)) == 0);
  mesh_rekey::wipe(inst);
  std::printf("PASS test_rekey_session_as_initiator\n");
}

void test_rekey_session_as_survivor() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x81 + i);
  uint8_t b_pub[32], b_priv[32];
  stand_up_session(S, b_pub, b_priv);          /* this device: B, a survivor */
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_commits.clear();

  uint8_t i_pub[32], i_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(i_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  uint8_t fp_b[8], fp_i[8], fp_x[8];
  mesh_crypto::compute_fingerprint(b_pub, fp_b);
  mesh_crypto::compute_fingerprint(i_pub, fp_i);
  mesh_crypto::compute_fingerprint(x_pub, fp_x);
  uint8_t old_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(old_id));

  /* The initiator's side. */
  mesh_rekey::Context ci;
  mesh_rekey::context_init(ci);
  const uint8_t surv[1][8] = {{fp_b[0], fp_b[1], fp_b[2], fp_b[3], fp_b[4], fp_b[5], fp_b[6], fp_b[7]}};
  mesh_rekey::Action offer = mesh_rekey::start(ci, fp_i, fp_x, surv, 1, 555, 0);
  assert(offer.type == mesh_rekey::ActionType::BROADCAST_OFFER);

  const uint8_t mac_i[6] = {0x02, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D};
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(i_pub, i_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                           offer.payload, offer.payload_len, frame, sizeof(frame));
  g_outs.clear();
  inject_from(mac_i, frame, flen);
  assert(mesh_session::rekey_in_progress());
  /* ACCEPT back to the initiator's MAC, under the old opera_id. */
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_i, 6) == 0);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(parse_session_frame(g_outs[0].bytes, b_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACCEPT));
  mesh_rekey::Action sec = mesh_rekey::receive(ci, fp_i, mesh_rekey::MsgType::ACCEPT,
                                               fp_b, pl, plen, 0);
  assert(sec.type == mesh_rekey::ActionType::SEND_SECRET);

  flen = build_signed_session_frame(i_pub, i_priv, S, 2, mesh_envelope::MsgType::REKEY_SECRET,
                                    sec.payload, sec.payload_len, frame, sizeof(frame));
  g_outs.clear();
  inject_from(mac_i, frame, flen);
  /* THE ORDERING: exactly one frame — the ACK — signed under the OLD
   * opera_id, so the initiator (still on the old one) can verify it. */
  assert(g_outs.size() == 1);
  assert(parse_session_frame(g_outs[0].bytes, b_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACK));
  assert(std::memcmp(hdr.opera_id, old_id, sizeof(old_id)) == 0);
  /* ...and only then did B switch. */
  assert(!mesh_session::rekey_in_progress());
  mesh_rekey::Action commit = mesh_rekey::receive(ci, fp_i, mesh_rekey::MsgType::ACK,
                                                  fp_b, pl, plen, 0);
  assert(commit.type == mesh_rekey::ActionType::COMMIT);
  assert(g_commits.size() == 1);
  assert(std::memcmp(g_commits[0].secret, commit.new_secret, 32) == 0);
  uint8_t new_id[mesh_crypto::OPERA_ID_LEN], expect_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(new_id));
  mesh_crypto::compute_opera_id(commit.new_secret, expect_id);
  assert(std::memcmp(new_id, expect_id, sizeof(new_id)) == 0);
  /* B dropped the removed device and told the integration layer which
   * pubkey to take out of NVS; the initiator stays trusted. */
  assert(g_commits[0].forgotten.size() == 1);
  assert(std::memcmp(g_commits[0].forgotten[0].data(), x_pub, 32) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(!mesh_session::unregister_trusted_peer(fp_x));
  mesh_rekey::wipe(commit);
  std::printf("PASS test_rekey_session_as_survivor\n");
}

void test_rekey_refusals_and_forgeries() {
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_commits.clear();
  uint8_t y_pub[32], y_priv[32], z_pub[32], z_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  assert(mesh_crypto::ed25519_generate_keypair(z_pub, z_priv));
  uint8_t fp_y[8], fp_z[8], out[32];
  mesh_crypto::compute_fingerprint(y_pub, fp_y);
  mesh_crypto::compute_fingerprint(z_pub, fp_z);

  assert(mesh_session::remove_peer(fp_y, 0, out) == mesh_session::RemoveResult::NO_OPERA);
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xB1 + i);
  assert(mesh_session::set_opera_secret(S));
  assert(mesh_session::remove_peer(fp_y, 0, out) == mesh_session::RemoveResult::NOT_FOUND);
  assert(mesh_session::register_trusted_peer(y_pub));
  assert(mesh_session::register_trusted_peer(z_pub));
  mesh_session::set_enabled(false);
  assert(mesh_session::remove_peer(fp_y, 0, out) == mesh_session::RemoveResult::DISABLED);
  mesh_session::set_enabled(true);
  assert(mesh_session::trusted_peer_count() == 2);   /* refusals changed nothing */

  assert(mesh_session::remove_peer(fp_y, 10, out) == mesh_session::RemoveResult::STARTED);
  assert(mesh_session::remove_peer(fp_z, 11, out) == mesh_session::RemoveResult::IN_FLIGHT);
  assert(mesh_session::trusted_peer_count() == 1);   /* z untouched */
  /* Disabling mid-rotation drops it (the REST layer refuses first). */
  mesh_session::set_enabled(false);
  assert(!mesh_session::rekey_in_progress());
  mesh_session::set_enabled(true);

  /* Inbound REKEY_OFFERs that must not engage this device: from an
   * unregistered sender, and a forged one claiming a trusted sender. */
  uint8_t w_pub[32], w_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(w_pub, w_priv));
  uint8_t fp_w[8], fp_me[8];
  mesh_crypto::compute_fingerprint(w_pub, fp_w);
  mesh_crypto::compute_fingerprint(pub, fp_me);
  mesh_rekey::Context cw;
  mesh_rekey::context_init(cw);
  const uint8_t surv[1][8] = {{fp_me[0], fp_me[1], fp_me[2], fp_me[3], fp_me[4], fp_me[5], fp_me[6], fp_me[7]}};
  mesh_rekey::Action offer = mesh_rekey::start(cw, fp_w, fp_y, surv, 1, 9, 0);
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(w_pub, w_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                           offer.payload, offer.payload_len, frame, sizeof(frame));
  const uint8_t mac_w[6] = {0x02, 0x3A, 0x3A, 0x3A, 0x3A, 0x3A};
  g_outs.clear();
  inject_from(mac_w, frame, flen);
  assert(g_outs.empty());
  assert(!mesh_session::rekey_in_progress());
  /* Signed by Z's key but tampered after signing. */
  mesh_rekey::Context cz;
  mesh_rekey::context_init(cz);
  offer = mesh_rekey::start(cz, fp_z, fp_y, surv, 1, 10, 0);
  flen = build_signed_session_frame(z_pub, z_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                    offer.payload, offer.payload_len, frame, sizeof(frame));
  frame[mesh_session::MSGTYPE_HEADER_LEN + mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  inject_from(mac_w, frame, flen);
  assert(g_outs.empty());
  assert(!mesh_session::rekey_in_progress());
  std::printf("PASS test_rekey_refusals_and_forgeries\n");
}

void test_rekey_timeout_and_no_survivors() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xC1 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_commits.clear();
  uint8_t b_pub[32], b_priv[32], c_pub[32], c_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(c_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  uint8_t fp_x[8], out[32];
  mesh_crypto::compute_fingerprint(x_pub, fp_x);

  const uint32_t t0 = 5000;
  assert(mesh_session::remove_peer(fp_x, t0, out) == mesh_session::RemoveResult::STARTED);
  mesh_session::process(t0 + mesh_rekey::REKEY_TIMEOUT_MS - 1);
  assert(g_commits.empty());
  mesh_session::process(t0 + mesh_rekey::REKEY_TIMEOUT_MS);
  /* Nobody answered: commit anyway, forgetting both silent survivors. */
  assert(g_commits.size() == 1);
  assert(g_commits[0].forgotten.size() == 2);
  assert(mesh_session::trusted_peer_count() == 0);
  assert(!mesh_session::rekey_in_progress());
  uint8_t id[16], expect[16];
  assert(mesh_session::get_opera_id(id));
  mesh_crypto::compute_opera_id(g_commits[0].secret, expect);
  assert(std::memcmp(id, expect, 16) == 0);
  /* A dropped survivor can be re-registered after it re-pairs. */
  assert(mesh_session::register_trusted_peer(b_pub));

  /* No survivors at all: rotate locally, at once. */
  g_commits.clear();
  uint8_t fp_b[8];
  mesh_crypto::compute_fingerprint(b_pub, fp_b);
  assert(mesh_session::remove_peer(fp_b, t0, out) == mesh_session::RemoveResult::COMMITTED);
  assert(g_commits.size() == 1);
  assert(g_commits[0].forgotten.empty());
  assert(!mesh_session::rekey_in_progress());
  assert(mesh_session::trusted_peer_count() == 0);
  assert(mesh_session::get_opera_id(id));
  mesh_crypto::compute_opera_id(g_commits[0].secret, expect);
  assert(std::memcmp(id, expect, 16) == 0);
  std::printf("PASS test_rekey_timeout_and_no_survivors\n");
}

void test_parse_fingerprint_hex() {
  uint8_t fp[8];
  std::memset(fp, 0xEE, sizeof(fp));
  assert(mesh_api::parse_fingerprint_hex("0011223344556677", fp));
  const uint8_t want[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  assert(std::memcmp(fp, want, 8) == 0);
  assert(mesh_api::parse_fingerprint_hex("AABBCCDDEEFF0011", fp));
  assert(fp[0] == 0xAA && fp[7] == 0x11);
  uint8_t keep[8];
  std::memcpy(keep, fp, 8);
  assert(!mesh_api::parse_fingerprint_hex("001122334455667", fp));     /* 15 */
  assert(!mesh_api::parse_fingerprint_hex("00112233445566778", fp));   /* 17 */
  assert(!mesh_api::parse_fingerprint_hex("00112233445566zz", fp));
  assert(!mesh_api::parse_fingerprint_hex("", fp));
  assert(!mesh_api::parse_fingerprint_hex(nullptr, fp));
  assert(std::memcmp(fp, keep, 8) == 0);   /* untouched on false */
  std::printf("PASS test_parse_fingerprint_hex\n");
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
  test_send_beacon_event_rejected_without_opera_secret();
  test_send_beacon_event_signs_and_broadcasts();
  test_send_beacon_event_counter_monotonic();
  test_deinit_clears_opera_auth_state();
  test_get_paired_peer_pubkey_gated_by_state();
  test_register_trusted_peer_basic();
  test_beacon_event_roundtrip();
  test_beacon_event_replay_dropped();
  test_beacon_event_unknown_sender_dropped();
  test_beacon_event_forged_signature_dropped();
  test_peer_link_mac_binding();
  /* PR-8 — status accessors + REST API JSON builders. */
  test_get_opera_id_matches_compute();
  test_set_get_opera_name_ram_only();
  test_mesh_state_name_mapping();
  test_build_mesh_status_json_active();
  test_build_mesh_status_json_pairing_code_only_in_confirm();
  test_build_mesh_status_json_escapes_name();
  test_build_mesh_status_json_no_opera_empty_id();
  test_build_mesh_peers_json();
  test_build_mesh_json_buffer_too_small();
  /* F10 — enable, leave, the alerts channel; F11 attribution. */
  test_tamper_alert_roundtrip();
  test_alert_ring_wraps_newest_first();
  test_send_tamper_alert();
  test_enable_disable();
  test_leave_opera();
  test_peer_left_dispatch();
  test_build_mesh_alerts_json();
  test_build_mesh_status_json_disabled();
  test_rest_buffers_fit_worst_case();
  /* F10-rekey — remove_peer + rotation (CRYPTO: maintainer review). */
  test_rekey_session_as_initiator();
  test_rekey_session_as_survivor();
  test_rekey_refusals_and_forgeries();
  test_rekey_timeout_and_no_survivors();
  test_parse_fingerprint_hex();
  std::printf("\nALL MESH_SESSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
