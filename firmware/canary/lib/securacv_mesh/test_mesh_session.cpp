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
  std::printf("\nALL MESH_SESSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
