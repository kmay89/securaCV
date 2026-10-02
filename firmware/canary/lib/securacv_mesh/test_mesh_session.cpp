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
#include "mesh_revocation.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
mesh_pairing::FailReason g_failed_why = mesh_pairing::FailReason::NONE;
std::vector<uint8_t>     g_failed_fp;
void on_failed(mesh_pairing::FailReason why, const uint8_t* fp) {
  g_failed_fired = true;
  g_failed_why = why;
  if (fp != nullptr) g_failed_fp.assign(fp, fp + mesh_crypto::FINGERPRINT_LEN);
  else g_failed_fp.clear();
}
void on_code_ready(uint32_t code) { g_code_ready = code; }

void reset_world() {
  mesh_session::deinit();
  mesh_transport::deinit();
  g_outs.clear();
  g_paired_fired = false;
  g_paired_with_secret = false;
  g_paired_code = 0;
  g_failed_fired = false;
  g_failed_why = mesh_pairing::FailReason::NONE;
  g_failed_fp.clear();
  g_code_ready = 0;
  std::memset(g_paired_secret, 0, sizeof(g_paired_secret));

  mesh_transport::test::set_now_ms(0);
  /* Each test starts with no send history: the limiter's window would
   * otherwise span every test run at virtual time 0. */
  mesh_transport::test::reset_storm_limiter();
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

/* A trusted member bound to `mac`. The opera senders reach bound members
 * only (F101), not a bare transport address, so a test that wants a send
 * to land adds one of these rather than mesh_transport::add_peer(). */
void add_bound_member(const uint8_t mac[6]) {
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  assert(mesh_session::register_trusted_peer(pub));
  mesh_crypto::compute_fingerprint(pub, fp);
  assert(mesh_session::bind_peer_mac(fp, mac));
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
  /* Envelope: byte 0 = PAIR_DISCOVER (8, the registry's). */
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

void test_joiner_offer_surfaces_code_with_accept() {
  /* F49 part 2: the joiner derives the session and the 6-digit code when
   * the OFFER lands, and its only action is SEND_ACCEPT — there is no
   * separate NOTIFY_CODE_READY on this side, so the CodeReadyCallback
   * used to fire for the initiator alone. Pin: an OFFER into a joiner
   * fires the callback once, with the same code pairing_confirmation_code()
   * (what GET /api/mesh exposes in PAIRING_CONFIRM) reports, in the same
   * beat as the ACCEPT frame going to the wire. */
  reset_world();
  assert(mesh_session::start_pairing_joiner(/*now_ms=*/100));
  g_outs.clear();
  assert(g_code_ready == 0);

  const uint8_t initiator_mac[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x01};
  assert(mesh_transport::add_peer(initiator_mac));

  /* The initiator's OFFER: its ephemeral + device pubkeys. Fresh keys are
   * enough — the joiner's derivation only needs valid curve points. */
  uint8_t i_pub[mesh_crypto::PUBKEY_LEN], i_priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t i_eph_pub[mesh_crypto::PUBKEY_LEN], i_eph_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  assert(mesh_crypto::x25519_generate_keypair(i_eph_pub, i_eph_priv));
  mesh_pairing::PairOfferPayload offer{};
  std::memcpy(offer.ephemeral_pubkey, i_eph_pub, mesh_crypto::PUBKEY_LEN);
  std::memcpy(offer.device_pubkey,    i_pub,     mesh_crypto::PUBKEY_LEN);
  std::snprintf(offer.opera_name, sizeof(offer.opera_name), "TheirOpera");

  uint8_t frame[1 + sizeof(offer)];
  frame[0] = static_cast<uint8_t>(mesh_session::MsgType::PAIR_OFFER);
  std::memcpy(frame + 1, &offer, sizeof(offer));
  mesh_transport::test::inject_recv(initiator_mac, frame, sizeof(frame), -50);
  mesh_transport::process();

  /* The ACCEPT went out, to the initiator. */
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, initiator_mac, 6) == 0);
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_ACCEPT));
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);

  /* And the code reached the UI callback, matching the API's report. */
  assert(g_code_ready != 0);
  assert(g_code_ready == mesh_session::pairing_confirmation_code());

  std::printf("PASS test_joiner_offer_surfaces_code_with_accept  (code=%u)\n",
              (unsigned)g_code_ready);
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
  frame[0] = static_cast<uint8_t>(mesh_session::MsgType::PAIR_DISCOVER);  /* 8 */
  std::memcpy(frame + 1, &disc, sizeof(disc));
  mesh_transport::test::inject_recv(joiner_mac, frame, sizeof(frame), -50);
  mesh_transport::process();

  assert(g_outs.size() == 1);
  /* Envelope check: byte 0 == MsgType::PAIR_OFFER (9, the registry's). */
  assert(g_outs[0].bytes[0] == 9);
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_OFFER));
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

  /* Add a member, bound to its radio MAC, so the send has a target. */
  uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  add_bound_member(peer_mac);

  /* Send. */
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED,
                                         "kitchen", /*now_ms=*/12345));

  /* One frame should have been captured by the send hook. */
  assert(g_outs.size() == 1);
  const auto& f = g_outs[0];

  /* Wire shape (spec §4.5): [Header(38B)] [Payload(25B)] [Sig(64B)]
   * = 127 bytes total, version byte first, the type INSIDE the signed
   * header — no unsigned prefix (v0.4 dropped the one this tree had). */
  const size_t expected_len = mesh_envelope::HEADER_LEN
      + mesh_beacon::PAYLOAD_LEN
      + mesh_envelope::SIGNATURE_LEN;
  assert(f.bytes.size() == expected_len);
  assert(f.bytes[mesh_envelope::OFFSET_VERSION]  == mesh_envelope::OPERA_VERSION);
  assert(f.bytes[mesh_envelope::OFFSET_MSG_TYPE] == static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT));

  /* Verify the signed envelope — the frame itself. */
  mesh_envelope::Header  hdr;
  const uint8_t*         payload = nullptr;
  size_t                 payload_len = 0;
  assert(mesh_envelope::parse_and_verify(
      f.bytes.data(), f.bytes.size(),
      pub, &hdr, &payload, &payload_len));
  assert(hdr.version  == mesh_envelope::OPERA_VERSION);
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
  const size_t cnt_off = mesh_envelope::OFFSET_COUNTER;
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
  h.version  = mesh_envelope::OPERA_VERSION;
  h.msg_type = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  mesh_crypto::compute_opera_id(opera_secret, h.opera_id);
  mesh_crypto::compute_fingerprint(sender_pub, h.sender_fp);
  h.counter   = counter;
  h.timestamp = 12345;

  /* Serialize+sign: the envelope is the whole frame (spec §4.5). */
  if (out_cap < mesh_envelope::MAX_FRAME_LEN) return 0;
  return mesh_envelope::serialize_signed(
      h, payload, sizeof(payload), sender_priv, sender_pub,
      out_frame, out_cap);
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
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/7,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      frame, sizeof(frame));
  assert(flen > 0);

  uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  /* mesh_transport's drain_ring only forwards frames whose source MAC
   * is in its table, and the session takes a member's frame only from the
   * member's own binding (F70): bind the sender there, as its pairing (or
   * the boot restore) does. */
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  assert(mesh_session::bind_peer_mac(tx_fp, mac));
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

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "replay",
      frame, sizeof(frame));
  assert(flen > 0);

  /* A unicast address: bind_peer_mac refuses a group one (0x11's I/G bit). */
  uint8_t mac[6] = {0x12, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  assert(mesh_session::bind_peer_mac(tx_fp, mac));   /* its binding (F70) */
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  /* Inject the IDENTICAL frame again. */
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();

  /* Only one event should have been delivered. */
  assert(g_received.size() == 1);

  /* A NEWER counter from the same peer DOES pass through. */
  uint8_t frame2[mesh_envelope::MAX_FRAME_LEN];
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

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
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

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
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
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;

  /* From the sender's own binding, so the frame passes the source gate
   * (F70) and the signature check is what refuses it. (A unicast address:
   * the old 77:77:.. is a group one, which bind_peer_mac refuses.) */
  uint8_t mac[6] = {0x02, 0x77, 0x77, 0x77, 0x77, 0x77};
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  assert(mesh_session::bind_peer_mac(tx_fp, mac));
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  assert(g_received.empty());

  /* Control: the same frame unflipped, from the same address, lands — the
   * drop above was the signature, not the route. */
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  mesh_transport::test::inject_recv(mac, frame, flen, -55);
  mesh_transport::process();
  assert(g_received.size() == 1);
  std::printf("PASS test_beacon_event_forged_signature_dropped\n");
}

void test_peer_link_mac_binding() {
  /* get_peer_links: a peer's link is its bound radio MAC, reported once a
   * fully verified frame has arrived from it — never on an unverified one,
   * and never on a frame from any other address. Until F70 the link was the
   * source of the peer's last verified frame, and a verified frame from
   * another address the transport table held moved it there. (An address
   * the table does not hold is never heard at all: the F49 part 3 tests in
   * the transport-table section below.) */
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
  g_received.clear();
  mesh_session::set_beacon_event_handler(on_beacon_event_received);

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN];
  uint8_t tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));

  uint8_t expected_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, expected_fp);

  /* Before any frame: the entry is listed but its MAC is unknown — also
   * once it is bound (the boot restore), until it is heard there. */
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(std::memcmp(links[0].fp, expected_fp, sizeof(expected_fp)) == 0);
  assert(!links[0].mac_known);
  uint8_t mac_a[6] = {0x02, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
  assert(mesh_session::bind_peer_mac(expected_fp, mac_a));
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);

  /* A frame whose signature does NOT verify must not mark the link —
   * otherwise anyone on the channel could relabel a peer's liveness. It
   * comes from the peer's own binding, so the signature is what refuses
   * it (until F70 it came from a hand-added address, which now drops
   * earlier, at the source). */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/1,
      mesh_beacon::BeaconState::ARRIVED, "forged",
      frame, sizeof(frame));
  assert(flen > 0);
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  mesh_transport::test::inject_recv(mac_a, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);
  assert(g_received.empty());

  /* A verified frame from the binding marks it. */
  flen = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/2,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      frame, sizeof(frame));
  assert(flen > 0);
  mesh_transport::test::inject_recv(mac_a, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].mac_known);
  assert(std::memcmp(links[0].mac, mac_a, sizeof(mac_a)) == 0);
  assert(g_received.size() == 1);

  /* A fresh, verified frame from another address the transport table
   * holds (added by hand: what a pairing partner's address, or another
   * member's, is to this peer) is dropped unread, and the link stays.
   * Until F70 it was dispatched and moved the link there. */
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
  assert(std::memcmp(links[0].mac, mac_a, sizeof(mac_a)) == 0);
  assert(g_received.size() == 1);

  /* A re-bind (a re-pair) moves the link with the binding; the peer is not
   * heard at the new address until a frame arrives from it, and the frame
   * dropped above spent no counter, so it lands there (after the two
   * refusals just below). */
  assert(mesh_session::bind_peer_mac(expected_fp, mac_b));
  assert(!mesh_transport::has_peer(mac_a));
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);
  assert(mesh_session::online_peer_count() == 0);

  /* Only a fresh frame of THIS opera marks the new binding. From mac_b, a
   * replay of the spent counter-2 frame and a frame signed for another
   * opera (counter 9, unspent) both verify, and both mark nothing,
   * dispatch nothing and spend nothing: the counter-3 frame still lands
   * below. (The base's "a REPLAYED frame from a different MAC must not
   * rebind" asserted the replay check ran before the link was recorded;
   * that frame now drops at the source, so the replay and opera_id checks
   * are pinned here, at the binding.) */
  uint8_t stale[mesh_envelope::MAX_FRAME_LEN];
  size_t stale_len = build_beacon_frame(
      tx_pub, tx_priv, opera_secret, /*counter=*/2,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      stale, sizeof(stale));
  assert(stale_len > 0);
  mesh_transport::test::inject_recv(mac_b, stale, stale_len, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);
  assert(mesh_session::online_peer_count() == 0);
  assert(g_received.size() == 1);
  uint8_t other_opera[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(other_opera); ++i) other_opera[i] = (uint8_t)(0x5A + i);
  stale_len = build_beacon_frame(
      tx_pub, tx_priv, other_opera, /*counter=*/9,
      mesh_beacon::BeaconState::ARRIVED, "kitchen",
      stale, sizeof(stale));
  assert(stale_len > 0);
  mesh_transport::test::inject_recv(mac_b, stale, stale_len, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(!links[0].mac_known);
  assert(mesh_session::online_peer_count() == 0);
  assert(g_received.size() == 1);

  mesh_transport::test::inject_recv(mac_b, frame, flen, -55);
  mesh_transport::process();
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].mac_known);
  assert(std::memcmp(links[0].mac, mac_b, sizeof(mac_b)) == 0);
  assert(g_received.size() == 2);
  assert(mesh_session::online_peer_count() == 1);

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
  add_bound_member(peer_mac);
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
  const size_t cnt_off = mesh_envelope::OFFSET_COUNTER;
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
  h.version  = mesh_envelope::OPERA_VERSION;
  h.msg_type = static_cast<uint8_t>(type);
  mesh_crypto::compute_opera_id(opera_secret, h.opera_id);
  mesh_crypto::compute_fingerprint(sender_pub, h.sender_fp);
  h.counter   = counter;
  h.timestamp = 12345;
  if (out_cap < mesh_envelope::MAX_FRAME_LEN) return 0;
  return mesh_envelope::serialize_signed(
      h, payload, payload_len, sender_priv, sender_pub,
      out_frame, out_cap);
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
  assert(mesh_session::bind_peer_mac(tx_fp, mac));   /* its binding (F70) */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_alert_frame(tx_pub, tx_priv, secret, /*counter=*/5,
                                  mesh_alert::Kind::CAMERA_TAMPER, 6, 777,
                                  frame, sizeof(frame));
  assert(flen == mesh_envelope::HEADER_LEN + mesh_alert::PAYLOAD_LEN
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
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
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
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  assert(mesh_session::bind_peer_mac(tx_fp, mac));   /* its binding (F70) */
  const size_t total = mesh_session::MAX_ALERT_HISTORY + 3;
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
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
  add_bound_member(peer_mac);

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
  assert(f[mesh_envelope::OFFSET_VERSION]  == mesh_envelope::OPERA_VERSION);
  assert(f[mesh_envelope::OFFSET_MSG_TYPE] == static_cast<uint8_t>(mesh_envelope::MsgType::TAMPER_ALERT));
  mesh_envelope::Header hdr;
  const uint8_t* payload = nullptr;
  size_t plen = 0;
  assert(mesh_envelope::parse_and_verify(f.data(), f.size(), pub,
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

  /* Inbound verified frames are not dispatched while disabled — from the
   * peer's own binding, so it is the disable that drops them (F70). */
  const uint8_t mac[6] = {0x02, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5};
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  assert(mesh_session::bind_peer_mac(tx_fp, mac));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
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

/* v0.4 (spec §4.5 — awaiting crypto review, not bench-verified): the frame
 * on the air is the registry's, and nothing else is taken for it.
 *   1. A frame written byte by byte the way canary-wap's send_to_peer()
 *      writes it (version, type, opera_id, fp, counter LE, timestamp LE,
 *      payload, Ed25519 over the rest under DOMAIN_MESSAGE) — with no
 *      call into mesh_envelope — is verified and dispatched here: the
 *      outer frames of the two trees are byte-compatible. LEAVE_OPERA is
 *      the vehicle because its (empty) payload is the same in both trees.
 *   2. Replayed, it is dropped (the counter is per fingerprint, whatever
 *      built the frame).
 *   3. Negative: the pre-v0.4 PIO shape ([unsigned type][envelope]) is
 *      dropped; a version-0 frame (canary-wap's old byte — a downgrade) is
 *      dropped even when everything after it verifies under version 1's
 *      layout; a canary-wap session-layer type (AUTH_CHALLENGE, 30) in a
 *      valid envelope is not dispatched as anything; a pairing-type first
 *      byte on a signed-length body never reaches the opera dispatch. None
 *      of the drops advances the peer's replay counter, so the sender's
 *      next honest frame is still accepted. */
void test_outer_frame_is_the_registry_frame() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x71 + i);
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, a_pub, a_priv);
  mesh_session::set_peer_left_handler(on_peer_left);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  uint8_t b_pub[mesh_crypto::PUBKEY_LEN], b_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  const uint8_t b_mac[6] = {0x02, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5};
  uint8_t b_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  /* B's frames come from its binding (F70); each re-registration below is
   * a re-pair, which binds it again. */
  assert(mesh_session::bind_peer_mac(b_fp, b_mac));
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(secret, opera_id);

  /* canary-wap's send_to_peer(), field by field, no mesh_envelope call. */
  auto wap_frame = [&](uint8_t version, uint8_t type, uint64_t counter,
                       const uint8_t* payload, size_t payload_len,
                       std::vector<uint8_t>* out) {
    out->clear();
    out->push_back(version);
    out->push_back(type);
    out->insert(out->end(), opera_id, opera_id + mesh_crypto::OPERA_ID_LEN);
    out->insert(out->end(), b_fp, b_fp + mesh_crypto::FINGERPRINT_LEN);
    for (int i = 0; i < 8; i++) out->push_back((uint8_t)((counter >> (i * 8)) & 0xFF));
    const uint32_t ts = 4242;
    for (int i = 0; i < 4; i++) out->push_back((uint8_t)((ts >> (i * 8)) & 0xFF));
    if (payload_len) out->insert(out->end(), payload, payload + payload_len);
    uint8_t sig[mesh_crypto::SIGNATURE_LEN];
    assert(mesh_crypto::ed25519_sign(b_priv, b_pub, out->data(), out->size(), sig));
    out->insert(out->end(), sig, sig + sizeof(sig));
  };
  std::vector<uint8_t> f;

  /* 3a. Downgrade: version 0 in front of an otherwise valid frame. */
  wap_frame(0, mesh_wire::LEAVE_OPERA, 1, nullptr, 0, &f);
  assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 1);

  /* 3b. The pre-v0.4 PIO shape: an unsigned type byte, then the envelope. */
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::LEAVE_OPERA, 1, nullptr, 0, &f);
  f.insert(f.begin(), mesh_wire::LEAVE_OPERA);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 1);

  /* 3c. A pairing-type first byte on a signed-length body: not a pairing
   * frame the state machine takes (no pairing runs, wrong length) and
   * never an opera frame. */
  wap_frame(mesh_wire::PAIR_OFFER, mesh_wire::LEAVE_OPERA, 1, nullptr, 0, &f);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.empty() && g_outs.empty());
  assert(mesh_session::trusted_peer_count() == 1);

  /* 1. The registry's frame, as canary-wap writes it, at the SAME counter
   * the three drops above carried: none of them reached verification, so
   * none consumed it. Also the same type byte canary-wap now sends for a
   * leave — 25, not its old 13. */
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::LEAVE_OPERA, 1, nullptr, 0, &f);
  assert(f[mesh_envelope::OFFSET_VERSION]  == 1);
  assert(f[mesh_envelope::OFFSET_MSG_TYPE] == 25);
  /* The same bytes verify through mesh_envelope's own parser first. */
  {
    mesh_envelope::Header hdr;
    const uint8_t* pl = nullptr; size_t plen = 9;
    assert(mesh_envelope::parse_and_verify(f.data(), f.size(), b_pub, &hdr, &pl, &plen));
    assert(plen == 0 && hdr.counter == 1 && hdr.timestamp == 4242);
    assert(std::memcmp(hdr.sender_fp, b_fp, sizeof(b_fp)) == 0);
  }
  const std::vector<uint8_t> first = f;
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.size() == 1);
  assert(std::memcmp(g_left[0].fp, b_fp, sizeof(b_fp)) == 0);
  assert(mesh_session::trusted_peer_count() == 0);

  /* 2. Replayed after a re-registration: the tombstone holds counter 1. */
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::bind_peer_mac(b_fp, b_mac));
  inject_from(b_mac, first.data(), first.size());
  assert(g_left.size() == 1);
  assert(mesh_session::trusted_peer_count() == 1);
  /* And counter 2 from the same sender is still live. */
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::LEAVE_OPERA, 2, nullptr, 0, &f);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.size() == 2);
  assert(mesh_session::trusted_peer_count() == 0);

  /* 3d. canary-wap's session layer (AUTH_CHALLENGE = 30): a valid signed
   * frame this tree has no handler for. Not a leave, not an alert — but
   * it IS a verified frame from that peer, so it advances the replay
   * counter like any other (existing behavior, stated): counter 3 is
   * spent, and a later frame at 3 is a replay; 4 is live. */
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::bind_peer_mac(b_fp, b_mac));
  const uint8_t junk[64] = {0};
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::AUTH_CHALLENGE, 3, junk, sizeof(junk), &f);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.size() == 2 && g_alerts_rx.empty());
  assert(mesh_session::trusted_peer_count() == 1);
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::LEAVE_OPERA, 3, nullptr, 0, &f);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.size() == 2);
  assert(mesh_session::trusted_peer_count() == 1);
  wap_frame(mesh_wire::OPERA_VERSION, mesh_wire::LEAVE_OPERA, 4, nullptr, 0, &f);
  inject_from(b_mac, f.data(), f.size());
  assert(g_left.size() == 3);
  assert(mesh_session::trusted_peer_count() == 0);

  /* Every opera frame THIS tree sends has the same first two bytes. (B's
   * last LEAVE dropped it and its bound address; bind a member there so
   * the send has somewhere to go: the senders reach bound members only.) */
  assert(mesh_session::set_opera_secret(secret));
  add_bound_member(b_mac);
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1, 9000));
  assert(g_outs.size() == 1);
  assert(g_outs[0].bytes[mesh_envelope::OFFSET_VERSION]  == mesh_wire::OPERA_VERSION);
  assert(g_outs[0].bytes[mesh_envelope::OFFSET_MSG_TYPE] == mesh_wire::TAMPER_ALERT);
  assert(g_outs[0].bytes.size() == mesh_envelope::HEADER_LEN + mesh_alert::PAYLOAD_LEN
                                   + mesh_envelope::SIGNATURE_LEN);
  std::printf("PASS test_outer_frame_is_the_registry_frame\n");
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
  uint8_t b_fp_bind[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(b_pub, b_fp_bind);
  assert(mesh_session::bind_peer_mac(b_fp_bind, b_mac));

  g_outs.clear();
  assert(mesh_session::leave_opera(5000));
  /* Exactly one frame: LEAVE_OPERA, empty payload, signed by A under the
   * opera_id it is leaving. */
  assert(g_outs.size() == 1);
  const std::vector<uint8_t> leave = g_outs[0].bytes;
  assert(leave[mesh_envelope::OFFSET_VERSION]  == mesh_envelope::OPERA_VERSION);
  assert(leave[mesh_envelope::OFFSET_MSG_TYPE] == static_cast<uint8_t>(mesh_envelope::MsgType::LEAVE_OPERA));
  assert(leave[mesh_envelope::OFFSET_MSG_TYPE] == 25);
  assert(leave.size() == mesh_envelope::MIN_FRAME_LEN);
  mesh_envelope::Header hdr;
  const uint8_t* payload = nullptr;
  size_t plen = 99;
  assert(mesh_envelope::parse_and_verify(leave.data(), leave.size(), a_pub,
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
  uint8_t a_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  assert(mesh_session::bind_peer_mac(a_fp, a_mac));   /* A's binding (F70) */
  inject_from(a_mac, leave.data(), leave.size());
  assert(g_left.size() == 1);
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

  /* X's binding (F70): every frame below claiming X comes from it, so each
   * refusal is the check it names, not the source gate. */
  const uint8_t mac[6] = {0x02, 0x11, 0x12, 0x13, 0x14, 0x15};
  assert(mesh_session::bind_peer_mac(x_fp, mac));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];

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

/* A frame whose header CLAIMS `claimed_fp` but is signed by signer_*. A
 * trusted peer forging on another trusted peer's behalf. */
size_t build_cross_signed_frame(const uint8_t claimed_fp[mesh_crypto::FINGERPRINT_LEN],
                                const uint8_t signer_pub[mesh_crypto::PUBKEY_LEN],
                                const uint8_t signer_priv[mesh_crypto::PRIVKEY_LEN],
                                const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                                uint64_t counter,
                                mesh_envelope::MsgType type,
                                const uint8_t* payload, size_t payload_len,
                                uint8_t* out_frame, size_t out_cap) {
  mesh_envelope::Header h;
  h.version  = mesh_envelope::OPERA_VERSION;
  h.msg_type = static_cast<uint8_t>(type);
  mesh_crypto::compute_opera_id(opera_secret, h.opera_id);
  std::memcpy(h.sender_fp, claimed_fp, mesh_crypto::FINGERPRINT_LEN);
  h.counter   = counter;
  h.timestamp = 12345;
  if (out_cap < mesh_envelope::MAX_FRAME_LEN) return 0;
  return mesh_envelope::serialize_signed(
      h, payload, payload_len, signer_priv, signer_pub,
      out_frame, out_cap);
}

/* Review finding (fw-mesh #2): "a verified frame only speaks for its own
 * signer" — which LEAVE's "removes only its signer" and REKEY_ACK counting
 * both rest on — had no test: a mutation verifying against ANY trusted
 * key, and one letting a LEAVE payload name its target, passed every test.
 * Here a TRUSTED peer Y forges on trusted X's behalf. */
void test_verified_frame_speaks_only_for_its_signer() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x6B + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  mesh_session::set_peer_left_handler(on_peer_left);
  mesh_session::set_tamper_alert_handler(on_alert_rx);

  uint8_t x_pub[mesh_crypto::PUBKEY_LEN], x_priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t y_pub[mesh_crypto::PUBKEY_LEN], y_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::register_trusted_peer(y_pub));
  uint8_t x_fp[mesh_crypto::FINGERPRINT_LEN], y_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  mesh_crypto::compute_fingerprint(y_pub, y_fp);
  /* X's binding (F70). Y's frames claiming X arrive from it — a radio can
   * send from any address — so it is X's key that refuses them; from Y's
   * own address they would not get that far. */
  const uint8_t mac[6] = {0x02, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B};
  assert(mesh_session::bind_peer_mac(x_fp, mac));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];

  /* (1) Y signs a LEAVE whose header claims X: X stays, no callback. */
  size_t flen = build_cross_signed_frame(x_fp, y_pub, y_priv, secret, 1,
                                         mesh_envelope::MsgType::LEAVE_OPERA,
                                         nullptr, 0, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);

  /* ...nor can Y attribute an alert to X. */
  uint8_t ap[mesh_alert::PAYLOAD_LEN];
  assert(mesh_alert::encode(mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 1, ap, sizeof(ap)));
  flen = build_cross_signed_frame(x_fp, y_pub, y_priv, secret, 2,
                                  mesh_envelope::MsgType::TAMPER_ALERT,
                                  ap, sizeof(ap), frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_alerts_rx.empty());
  assert(mesh_session::alerts_received() == 0);

  /* (2) X signs a LEAVE whose 8-byte payload names Y: a LEAVE carries no
   * target, so nothing is removed — not Y, and not X either. */
  flen = build_signed_session_frame(x_pub, x_priv, secret, 3,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    y_fp, sizeof(y_fp), frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.empty());
  assert(mesh_session::trusted_peer_count() == 2);
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 2);

  /* Control: X's own, well-formed LEAVE removes X. */
  flen = build_signed_session_frame(x_pub, x_priv, secret, 4,
                                    mesh_envelope::MsgType::LEAVE_OPERA,
                                    nullptr, 0, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_left.size() == 1);
  assert(std::memcmp(g_left[0].fp, x_fp, sizeof(x_fp)) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  std::printf("PASS test_verified_frame_speaks_only_for_its_signer\n");
}

/* Review finding (fw-mesh #1): a LEAVE followed by a re-pair into the same,
 * un-rotated opera must not re-open the replay window. Before the fix the
 * receiver dropped X's last_counter with X, re-registration restarted it at
 * 0, and X's recorded pre-leave alert and LEAVE both verified again. */
void test_replay_tombstones_across_leave_and_repair() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x3D + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  mesh_session::set_peer_left_handler(on_peer_left);
  mesh_session::set_tamper_alert_handler(on_alert_rx);

  uint8_t x_pub[mesh_crypto::PUBKEY_LEN], x_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  uint8_t x_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  assert(mesh_session::register_trusted_peer(x_pub));
  const uint8_t mac[6] = {0x02, 0x4D, 0x4D, 0x4D, 0x4D, 0x4D};
  /* X's binding (F70); the re-pair and the reboot below bind it again, so
   * the replays reach the counter check they are about. */
  assert(mesh_session::bind_peer_mac(x_fp, mac));

  /* X alerts (counter 40), then leaves (counter 41); the receiver drops X. */
  uint8_t alert40[mesh_envelope::MAX_FRAME_LEN], leave41[mesh_envelope::MAX_FRAME_LEN];
  const size_t alert40_len = build_alert_frame(x_pub, x_priv, secret, 40,
                                               mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 7,
                                               alert40, sizeof(alert40));
  const size_t leave41_len = build_signed_session_frame(x_pub, x_priv, secret, 41,
                                                        mesh_envelope::MsgType::LEAVE_OPERA,
                                                        nullptr, 0, leave41, sizeof(leave41));
  inject_from(mac, alert40, alert40_len);
  inject_from(mac, leave41, leave41_len);
  assert(mesh_session::alerts_received() == 1);
  assert(g_left.size() == 1);
  assert(mesh_session::trusted_peer_count() == 0);

  /* X's counter outlives its trust entry, and is what gets persisted. */
  uint8_t fps[mesh_session::MAX_REPLAY_COUNTERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs[mesh_session::MAX_REPLAY_COUNTERS];
  static_assert(mesh_state::MAX_REPLAY_ENTRIES >= mesh_session::MAX_REPLAY_COUNTERS,
                "replay_ctrs must hold every live counter and tombstone");
  assert(mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS) == 1);
  assert(std::memcmp(fps[0], x_fp, sizeof(x_fp)) == 0);
  assert(ctrs[0] == 41);

  /* X re-pairs into the same opera: re-registered, counter resumes at 41. */
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::bind_peer_mac(x_fp, mac));
  assert(mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS) == 1);
  assert(ctrs[0] == 41);   /* the live entry now carries it; tombstone consumed */

  /* Both recorded frames are replays now. */
  inject_from(mac, alert40, alert40_len);
  assert(mesh_session::alerts_received() == 1);
  assert(g_alerts_rx.size() == 1);
  inject_from(mac, leave41, leave41_len);
  assert(g_left.size() == 1);
  assert(mesh_session::trusted_peer_count() == 1);

  /* X's genuine new traffic (its counter kept counting) still flows. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_alert_frame(x_pub, x_priv, secret, 42,
                                  mesh_alert::Kind::TEMP_DRIFT, 3, 8, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(mesh_session::alerts_received() == 2);

  /* Across a reboot (main.cpp restores every persisted replay_ctrs entry
   * after registering the persisted peers): an entry for a fingerprint that
   * is no longer trusted comes back as a tombstone, which a later
   * registration applies. */
  stand_up_session(secret, rx_pub, rx_priv);             /* "reboot": RAM gone */
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  assert(mesh_session::restore_replay_counter(x_fp, 42));
  assert(!mesh_session::restore_replay_counter(x_fp, 41));   /* never lowers */
  assert(mesh_session::trusted_peer_count() == 0);
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::bind_peer_mac(x_fp, mac));         /* NVS peer_macs */
  inject_from(mac, frame, flen);                          /* counter 42: replay */
  assert(g_alerts_rx.empty());
  flen = build_alert_frame(x_pub, x_priv, secret, 43,
                           mesh_alert::Kind::TEMP_DRIFT, 3, 9, frame, sizeof(frame));
  inject_from(mac, frame, flen);
  assert(g_alerts_rx.size() == 1);

  /* clear_trusted_peers() (what leave_opera uses) tombstones too. */
  mesh_session::clear_trusted_peers();
  assert(mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS) == 1);
  assert(ctrs[0] == 43);

  /* The tombstone table is bounded: past MAX_COUNTER_TOMBSTONES the oldest
   * goes, the rest stay. X (tombstoned first, just above) is the oldest. */
  for (size_t i = 0; i < mesh_session::MAX_COUNTER_TOMBSTONES; ++i) {
    uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
    std::memset(fp, (int)(0xA0 + i), sizeof(fp));
    assert(mesh_session::restore_replay_counter(fp, 100 + i));
  }
  const size_t n = mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS);
  assert(n == mesh_session::MAX_COUNTER_TOMBSTONES);
  for (size_t i = 0; i < n; ++i) assert(std::memcmp(fps[i], x_fp, sizeof(x_fp)) != 0);
  /* Reported oldest first, whatever slot each landed in (0xA7 took X's). */
  for (size_t i = 0; i < n; ++i) assert(fps[i][0] == 0xA0 + i);
  /* ...and a zero counter (a peer that never got a frame through) parks nothing. */
  uint8_t z_fp[mesh_crypto::FINGERPRINT_LEN];
  std::memset(z_fp, 0x5A, sizeof(z_fp));
  assert(!mesh_session::restore_replay_counter(z_fp, 0));
  std::printf("PASS test_replay_tombstones_across_leave_and_repair\n");
}

/* The persisted order keeps tombstone ages across a reboot: tombstones
 * oldest first, then the live counters. If a device leaves its opera and
 * reboots before the next save, its peers' LIVE counters come back as
 * tombstones — as the newest ones, so an overflow evicts older tombstones,
 * not them. */
void test_replay_tombstones_keep_age_across_reboot() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x2C + i);
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, pub, priv);

  /* 8 old tombstones (fps 0xB0.., counters 200..), then 8 live peers. */
  for (size_t i = 0; i < mesh_session::MAX_COUNTER_TOMBSTONES; ++i) {
    uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
    std::memset(fp, (int)(0xB0 + i), sizeof(fp));
    assert(mesh_session::restore_replay_counter(fp, 200 + i));
  }
  uint8_t live_pub[mesh_session::MAX_TRUSTED_PEERS][mesh_crypto::PUBKEY_LEN];
  uint8_t live_fp [mesh_session::MAX_TRUSTED_PEERS][mesh_crypto::FINGERPRINT_LEN];
  for (size_t i = 0; i < mesh_session::MAX_TRUSTED_PEERS; ++i) {
    uint8_t lp[mesh_crypto::PRIVKEY_LEN];
    assert(mesh_crypto::ed25519_generate_keypair(live_pub[i], lp));
    mesh_crypto::compute_fingerprint(live_pub[i], live_fp[i]);
    assert(mesh_session::register_trusted_peer(live_pub[i]));
    assert(mesh_session::restore_replay_counter(live_fp[i], 500 + i));
  }

  uint8_t fps[mesh_session::MAX_REPLAY_COUNTERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs[mesh_session::MAX_REPLAY_COUNTERS];
  const size_t n = mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS);
  assert(n == mesh_session::MAX_REPLAY_COUNTERS);
  for (size_t i = 0; i < mesh_session::MAX_COUNTER_TOMBSTONES; ++i) {
    assert(fps[i][0] == 0xB0 + i && ctrs[i] == 200 + i);   /* oldest first */
  }
  for (size_t i = mesh_session::MAX_COUNTER_TOMBSTONES; i < n; ++i) {
    assert(ctrs[i] >= 500);                                 /* then the live ones */
  }

  /* "Reboot" after a leave that NVS never saw: no trusted peers any more,
   * the whole blob restores in order, and the overflow evicts the OLD
   * tombstones — every former live peer keeps its counter. */
  stand_up_session(secret, pub, priv);
  for (size_t i = 0; i < n; ++i) mesh_session::restore_replay_counter(fps[i], ctrs[i]);
  uint8_t fps2[mesh_session::MAX_REPLAY_COUNTERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs2[mesh_session::MAX_REPLAY_COUNTERS];
  const size_t n2 = mesh_session::get_replay_counters(fps2, ctrs2, mesh_session::MAX_REPLAY_COUNTERS);
  assert(n2 == mesh_session::MAX_COUNTER_TOMBSTONES);
  for (size_t i = 0; i < n2; ++i) assert(ctrs2[i] >= 500);
  std::printf("PASS test_replay_tombstones_keep_age_across_reboot\n");
}

/* The leaver's side of the same fix: leave_opera() keeps the outbound
 * counter, so after a re-pair this device's frames continue ABOVE the
 * tombstone its peers hold for it instead of restarting at 1. */
void test_leave_keeps_outbound_counter() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x4E + i);
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, a_pub, a_priv);
  uint8_t b_pub[mesh_crypto::PUBKEY_LEN], b_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  const uint8_t b_mac[6] = {0x02, 0x5E, 0x5E, 0x5E, 0x5E, 0x5E};
  uint8_t b_fp_bind[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(b_pub, b_fp_bind);
  assert(mesh_session::bind_peer_mac(b_fp_bind, b_mac));

  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 100));   /* 1 */
  assert(mesh_session::leave_opera(200));                                               /* 2 */
  assert(g_outs.size() == 2);
  /* Re-pair into the same opera (which binds B's address again). */
  assert(mesh_session::set_opera_secret(secret));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::bind_peer_mac(b_fp_bind, b_mac));
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 300));
  assert(g_outs.size() == 1);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(mesh_envelope::parse_and_verify(g_outs[0].bytes.data(), g_outs[0].bytes.size(),
                                         a_pub, &hdr, &pl, &plen));
  assert(hdr.counter == 3);
  std::printf("PASS test_leave_keeps_outbound_counter\n");
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
  assert(mesh_api::build_mesh_alerts_json(buf, sizeof(buf), recs, 2, 150000));
  /* The exact field names the web UI's loadOperaAlerts() reads — and the
   * receiver's uptime each timestamp_ms is measured against (F33 part 7:
   * the UI shows 150000 - 90000 as "1 min ago", never a date). */
  assert(std::strstr(buf, "{\"ok\":true,\"count\":2,\"uptime_ms\":150000,\"alerts\":[{") == buf);
  assert(std::strstr(buf, "{\"timestamp_ms\":90000,\"type\":\"TAMPER\",\"severity\":6,"
                          "\"sender_fp\":\"1011121314151617\",\"sender_name\":\"\","
                          "\"detail\":\"camera_tamper\",\"witness_seq\":4242}") != nullptr);
  assert(std::strstr(buf, "\"detail\":\"unknown\"") != nullptr);

  /* Empty history is a valid envelope. */
  assert(mesh_api::build_mesh_alerts_json(buf, sizeof(buf), nullptr, 0, 7));
  assert(std::strcmp(buf, "{\"ok\":true,\"count\":0,\"uptime_ms\":7,\"alerts\":[]}") == 0);

  /* Overflow fails cleanly; null records with count>0 refused. */
  char tiny[16];
  assert(!mesh_api::build_mesh_alerts_json(tiny, sizeof(tiny), recs, 2, 0));
  assert(!mesh_api::build_mesh_alerts_json(buf, sizeof(buf), nullptr, 1, 0));
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
                                          mesh_api::MAX_ALERTS_JSON, 0xFFFFFFFFu));
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
  return mesh_envelope::parse_and_verify(f.data(), f.size(), signer_pub,
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

  /* Both peers are bound where they paired, and speak once from there.
   * (Until F70 their frames from hand-added addresses were taken, and that
   * recorded the address; a frame from anywhere but the binding now drops.) */
  const uint8_t mac_b[6] = {0x02, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B};
  const uint8_t mac_x[6] = {0x02, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C};
  assert(mesh_session::bind_peer_mac(fp_b, mac_b));
  assert(mesh_session::bind_peer_mac(fp_x, mac_x));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
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
  const std::vector<uint8_t> offer_pl(pl, pl + plen);

  /* Nothing heard for REKEY_RETRY_MS: process() re-broadcasts the same
   * OFFER (review fix — one lost frame no longer drops a survivor). */
  g_outs.clear();
  mesh_session::process(1000 + mesh_rekey::REKEY_RETRY_MS - 1);
  assert(g_outs.empty());
  mesh_session::process(1000 + mesh_rekey::REKEY_RETRY_MS);
  assert(g_outs.size() == 1);
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_OFFER));
  assert(plen == offer_pl.size() && std::memcmp(pl, offer_pl.data(), plen) == 0);
  pl = offer_pl.data();
  plen = offer_pl.size();

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
  /* Inside the settle window (F33 part 6) the ACCEPT is held; when it
   * closes, process() answers with B's SECRET, unicast to B's bound
   * MAC. */
  assert(g_outs.empty());
  mesh_session::process(1000 + mesh_rekey::REKEY_SETTLE_MS - 1);
  assert(g_outs.empty());
  mesh_session::process(1000 + mesh_rekey::REKEY_SETTLE_MS);
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
  mesh_state::test::reset_journal();
  flen = build_signed_session_frame(b_pub, b_priv, S, 3, mesh_envelope::MsgType::REKEY_ACK,
                                    inst.payload, inst.payload_len, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  assert(g_commits.size() == 1);
  assert(std::memcmp(g_commits[0].secret, inst.new_secret, 32) == 0);
  /* The commit hands over the removed X once more (it left the table at
   * start) so persist_rotation drops it from NVS BEFORE saving the new
   * secret: remove, then save (review fix — fail closed). */
  assert(g_commits[0].forgotten.size() == 1);
  assert(std::memcmp(g_commits[0].forgotten[0].data(), x_pub, 32) == 0);
  assert(g_commits[0].persisted);
  assert(std::strcmp(mesh_state::test::journal(), "RS") == 0);
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
   * per-fingerprint counters): OFFER=1, its retransmission=2, SECRET=3,
   * so the next is 4. */
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 2000));
  assert(g_outs.size() == 1);
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.counter == 4);
  assert(std::memcmp(hdr.opera_id, new_id, sizeof(new_id)) == 0);
  mesh_rekey::wipe(inst);
  std::printf("PASS test_rekey_session_as_initiator\n");
}

/* F33 part 6: peer_revoked_fn capture. */
struct Revoked {
  std::vector<uint8_t> fp;
  std::vector<uint8_t> pub;   /* empty when it was not trusted */
};
std::vector<Revoked> g_revoked;
void on_peer_revoked(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN], const uint8_t* pub) {
  Revoked r;
  r.fp.assign(fp, fp + mesh_crypto::FINGERPRINT_LEN);
  if (pub != nullptr) r.pub.assign(pub, pub + mesh_crypto::PUBKEY_LEN);
  g_revoked.push_back(r);
}

void test_rekey_session_as_survivor() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x81 + i);
  uint8_t b_pub[32], b_priv[32];
  stand_up_session(S, b_pub, b_priv);          /* this device: B, a survivor */
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  mesh_session::set_peer_revoked_handler(on_peer_revoked);
  g_commits.clear();
  g_revoked.clear();

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
  assert(mesh_session::bind_peer_mac(fp_i, mac_i));   /* its binding (F70) */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(i_pub, i_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                           offer.payload, offer.payload_len, frame, sizeof(frame));
  g_outs.clear();
  inject_from(mac_i, frame, flen);
  assert(mesh_session::rekey_in_progress());
  /* F33 part 6: the OFFER's removal holds here at once — X is forgotten,
   * deny-listed, and the integration layer told which pubkey to drop from
   * NVS — before (whether or not) B gets the new secret. */
  assert(mesh_session::trusted_peer_count() == 1);
  assert(mesh_session::is_revoked(fp_x));
  assert(g_revoked.size() == 1 && std::memcmp(g_revoked[0].fp.data(), fp_x, 8) == 0);
  assert(g_revoked[0].pub.size() == 32 && std::memcmp(g_revoked[0].pub.data(), x_pub, 32) == 0);
  /* ACCEPT back to the initiator's MAC, under the old opera_id. */
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_i, 6) == 0);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(parse_session_frame(g_outs[0].bytes, b_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACCEPT));
  mesh_rekey::Action sec = mesh_rekey::receive(ci, fp_i, mesh_rekey::MsgType::ACCEPT,
                                               fp_b, pl, plen, mesh_rekey::REKEY_SETTLE_MS);
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
  /* B dropped the removed device at the OFFER (above, through the
   * revocation handler), so the commit has nothing left to forget; the
   * initiator stays trusted. */
  assert(g_commits[0].forgotten.empty());
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
  assert(mesh_session::remove_peer(fp_y, 0, out) == mesh_session::RemoveResult::MESH_DISABLED);
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
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(w_pub, w_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                           offer.payload, offer.payload_len, frame, sizeof(frame));
  const uint8_t mac_w[6] = {0x02, 0x3A, 0x3A, 0x3A, 0x3A, 0x3A};
  g_outs.clear();
  inject_from(mac_w, frame, flen);
  assert(g_outs.empty());
  assert(!mesh_session::rekey_in_progress());
  /* Signed by Z's key but tampered after signing — from Z's own binding,
   * so the signature is what refuses it (F70: from W's address it would
   * drop before the signature check). */
  const uint8_t mac_z[6] = {0x02, 0x3B, 0x3B, 0x3B, 0x3B, 0x3B};
  assert(mesh_session::bind_peer_mac(fp_z, mac_z));
  mesh_rekey::Context cz;
  mesh_rekey::context_init(cz);
  offer = mesh_rekey::start(cz, fp_z, fp_y, surv, 1, 10, 0);
  flen = build_signed_session_frame(z_pub, z_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                    offer.payload, offer.payload_len, frame, sizeof(frame));
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  inject_from(mac_z, frame, flen);
  assert(g_outs.empty());
  assert(!mesh_session::rekey_in_progress());
  /* Control: untampered, from the same address, it engages this device. */
  frame[mesh_envelope::OFFSET_PAYLOAD] ^= 0x01;
  inject_from(mac_z, frame, flen);
  assert(mesh_session::rekey_in_progress());
  assert(g_outs.size() == 1 && std::memcmp(g_outs[0].mac, mac_z, 6) == 0);
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
  /* Nobody answered: commit anyway, forgetting both silent survivors —
   * after the removed X, which leads the list. */
  assert(g_commits.size() == 1);
  assert(g_commits[0].forgotten.size() == 3);
  assert(std::memcmp(g_commits[0].forgotten[0].data(), x_pub, 32) == 0);
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
  mesh_state::test::reset_journal();
  assert(mesh_session::remove_peer(fp_b, t0, out) == mesh_session::RemoveResult::COMMITTED);
  assert(g_commits.size() == 1);
  /* Even the at-once commit drops B from NVS before it saves the secret. */
  assert(g_commits[0].forgotten.size() == 1);
  assert(std::memcmp(g_commits[0].forgotten[0].data(), b_pub, 32) == 0);
  assert(std::strcmp(mesh_state::test::journal(), "RS") == 0);
  assert(!mesh_session::rekey_in_progress());
  assert(mesh_session::trusted_peer_count() == 0);
  assert(mesh_session::get_opera_id(id));
  mesh_crypto::compute_opera_id(g_commits[0].secret, expect);
  assert(std::memcmp(id, expect, 16) == 0);
  std::printf("PASS test_rekey_timeout_and_no_survivors\n");
}

/* (3) + (4) of the same finding, on the rotation: a trusted survivor Y
 * cannot ACK on survivor B's behalf (a partial ACK set must not commit
 * early and keep a silent B), and a replayed REKEY_ACCEPT / REKEY_OFFER is
 * dropped at the session's replay check (a mutation skipping it for
 * msg_type >= 26 passed every test). */
void test_rekey_frames_speak_only_for_their_signer_once() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x7C + i);
  uint8_t a_pub[32], a_priv[32];
  stand_up_session(S, a_pub, a_priv);          /* this device: A, the initiator */
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_commits.clear();
  uint8_t b_pub[32], b_priv[32], y_pub[32], y_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(y_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  uint8_t fp_a[8], fp_b[8], fp_y[8], fp_x[8];
  mesh_crypto::compute_fingerprint(a_pub, fp_a);
  mesh_crypto::compute_fingerprint(b_pub, fp_b);
  mesh_crypto::compute_fingerprint(y_pub, fp_y);
  mesh_crypto::compute_fingerprint(x_pub, fp_x);
  const uint8_t mac_b[6] = {0x02, 0x7B, 0x7B, 0x7B, 0x7B, 0x7B};
  const uint8_t mac_y[6] = {0x02, 0x79, 0x79, 0x79, 0x79, 0x79};
  /* Bound where they paired (F70: until then added by hand, which a
   * member's frames no longer come through). */
  assert(mesh_session::bind_peer_mac(fp_b, mac_b));
  assert(mesh_session::bind_peer_mac(fp_y, mac_y));

  g_outs.clear();
  uint8_t removed[32];
  assert(mesh_session::remove_peer(fp_x, 1000, removed) == mesh_session::RemoveResult::STARTED);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(!g_outs.empty());
  assert(parse_session_frame(g_outs.back().bytes, a_pub, &hdr, &pl, &plen));
  const std::vector<uint8_t> offer(pl, pl + plen);

  /* B and Y both accept and get their SECRETs. */
  mesh_rekey::Context cb, cy;
  mesh_rekey::context_init(cb);
  mesh_rekey::context_init(cy);
  mesh_rekey::Action acc_b = mesh_rekey::receive(cb, fp_b, mesh_rekey::MsgType::OFFER,
                                                 fp_a, offer.data(), offer.size(), 0);
  mesh_rekey::Action acc_y = mesh_rekey::receive(cy, fp_y, mesh_rekey::MsgType::OFFER,
                                                 fp_a, offer.data(), offer.size(), 0);
  assert(acc_b.type == mesh_rekey::ActionType::SEND_ACCEPT);
  assert(acc_y.type == mesh_rekey::ActionType::SEND_ACCEPT);
  uint8_t acc_b_frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t acc_b_len = build_signed_session_frame(b_pub, b_priv, S, 10,
                                                      mesh_envelope::MsgType::REKEY_ACCEPT,
                                                      acc_b.payload, acc_b.payload_len,
                                                      acc_b_frame, sizeof(acc_b_frame));
  g_outs.clear();
  inject_from(mac_b, acc_b_frame, acc_b_len);
  assert(g_outs.size() == 1);                   /* B's SECRET */

  /* (4a) The same ACCEPT again — same counter — is a replay: no second SECRET. */
  g_outs.clear();
  inject_from(mac_b, acc_b_frame, acc_b_len);
  assert(g_outs.empty());

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(y_pub, y_priv, S, 20,
                                           mesh_envelope::MsgType::REKEY_ACCEPT,
                                           acc_y.payload, acc_y.payload_len,
                                           frame, sizeof(frame));
  g_outs.clear();
  inject_from(mac_y, frame, flen);
  assert(g_outs.size() == 1);
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  mesh_rekey::Action inst_y = mesh_rekey::receive(cy, fp_y, mesh_rekey::MsgType::SECRET,
                                                  fp_a, pl, plen, 0);
  assert(inst_y.type == mesh_rekey::ActionType::ACK_AND_INSTALL);

  /* Y ACKs in its own name: B is still out, so no commit. */
  flen = build_signed_session_frame(y_pub, y_priv, S, 21, mesh_envelope::MsgType::REKEY_ACK,
                                    inst_y.payload, inst_y.payload_len, frame, sizeof(frame));
  inject_from(mac_y, frame, flen);
  assert(g_commits.empty());
  assert(mesh_session::rekey_in_progress());

  /* (3) Y forges B's ACK (header claims B, Y's signature): dropped. Were it
   * counted, A would commit now and keep trusting a B that never got the
   * secret. Y sends it from B's address (a radio can send from any): from
   * there it reaches the signature check, which refuses it under B's key.
   * From Y's own address it drops earlier, at the source (F70). */
  flen = build_cross_signed_frame(fp_b, y_pub, y_priv, S, 22, mesh_envelope::MsgType::REKEY_ACK,
                                  inst_y.payload, inst_y.payload_len, frame, sizeof(frame));
  inject_from(mac_b, frame, flen);
  assert(g_commits.empty());
  assert(mesh_session::rekey_in_progress());
  inject_from(mac_y, frame, flen);
  assert(g_commits.empty());
  assert(mesh_session::rekey_in_progress());

  /* The timeout commits and drops the silent B. */
  mesh_session::process(1000 + mesh_rekey::REKEY_TIMEOUT_MS);
  assert(g_commits.size() == 1);
  assert(!mesh_session::rekey_in_progress());
  assert(mesh_session::trusted_peer_count() == 1);      /* Y only */
  assert(!mesh_session::unregister_trusted_peer(fp_b));
  mesh_rekey::wipe(inst_y);

  /* (4b) Survivor side: a replayed OFFER after this device aborted does not
   * re-engage it. */
  uint8_t s_pub[32], s_priv[32];
  stand_up_session(S, s_pub, s_priv);          /* this device: a survivor */
  assert(mesh_session::register_trusted_peer(a_pub));
  uint8_t fp_s[8];
  mesh_crypto::compute_fingerprint(s_pub, fp_s);
  mesh_rekey::Context ci;
  mesh_rekey::context_init(ci);
  const uint8_t surv[1][8] = {{fp_s[0], fp_s[1], fp_s[2], fp_s[3], fp_s[4], fp_s[5], fp_s[6], fp_s[7]}};
  mesh_rekey::Action off = mesh_rekey::start(ci, fp_a, fp_x, surv, 1, 77, 0);
  assert(off.type == mesh_rekey::ActionType::BROADCAST_OFFER);
  const uint8_t mac_a[6] = {0x02, 0x7A, 0x7A, 0x7A, 0x7A, 0x7A};
  assert(mesh_session::bind_peer_mac(fp_a, mac_a));   /* its binding (F70) */
  flen = build_signed_session_frame(a_pub, a_priv, S, 5, mesh_envelope::MsgType::REKEY_OFFER,
                                    off.payload, off.payload_len, frame, sizeof(frame));
  g_outs.clear();
  mesh_session::process(2000);
  inject_from(mac_a, frame, flen);
  assert(mesh_session::rekey_in_progress());
  assert(g_outs.size() == 1);                   /* the ACCEPT */
  mesh_session::process(2000 + mesh_rekey::REKEY_TIMEOUT_MS);
  assert(!mesh_session::rekey_in_progress());   /* aborted: SECRET never came */
  g_outs.clear();
  inject_from(mac_a, frame, flen);              /* the recorded OFFER, again */
  assert(!mesh_session::rekey_in_progress());
  assert(g_outs.empty());
  std::printf("PASS test_rekey_frames_speak_only_for_their_signer_once\n");
}

/* Review finding (fw-mesh #3/#4): the F10 REST mutators ran on the httpd
 * task against state the main loop mutates. They now go through a
 * one-deep request slot that process() drains. Single-threaded here: the
 * test plays the httpd side (submit / take / withdraw / abandon) and the
 * main loop (process) in turn. */
bool g_abandon_in_commit = false;
void on_rekey_commit_abandoning(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN],
                                const uint8_t (*pks)[mesh_crypto::PUBKEY_LEN], size_t n) {
  on_rekey_commit(secret, pks, n);
  /* The handler gives up while the request is still RUNNING. */
  if (g_abandon_in_commit) mesh_session::abandon_request();
}

mesh_session::Request make_request(mesh_session::RequestType t) {
  mesh_session::Request r;
  std::memset(&r, 0, sizeof(r));
  r.type = t;
  return r;
}

void test_rest_request_slot() {
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::RequestResult res;

  /* Empty slot: nothing to take, nothing to withdraw. */
  assert(!mesh_session::take_request_result(&res));
  assert(!mesh_session::withdraw_request());

  /* Nothing runs until the main loop's process(); the slot is one deep. */
  mesh_session::Request name = make_request(mesh_session::RequestType::SET_NAME);
  std::strcpy(name.name, "Porch");
  assert(mesh_session::submit_request(name));
  assert(!mesh_session::submit_request(make_request(mesh_session::RequestType::CLEAR_ALERTS)));
  assert(!mesh_session::take_request_result(&res));
  mesh_session::process(10);
  assert(mesh_session::take_request_result(&res));
  assert(res.type == mesh_session::RequestType::SET_NAME);
  assert(res.status == mesh_session::RequestStatus::NO_OPERA);   /* no opera yet */
  assert(!mesh_session::take_request_result(&res));               /* taken once */

  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x93 + i);
  assert(mesh_session::set_opera_secret(S));
  assert(mesh_session::submit_request(name));
  mesh_session::process(20);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK);
  char got[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(got, sizeof(got));
  assert(std::strcmp(got, "Porch") == 0);

  /* A request withdrawn before the main loop took it never runs. */
  mesh_session::Request off = make_request(mesh_session::RequestType::SET_ENABLED);
  off.enabled = false;
  assert(mesh_session::submit_request(off));
  assert(mesh_session::withdraw_request());
  mesh_session::process(30);
  assert(!mesh_session::take_request_result(&res));
  assert(mesh_session::is_enabled());

  /* Enable runs even while the session is stopped (process drains first). */
  assert(mesh_session::submit_request(off));
  mesh_session::process(40);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && !res.enabled);
  assert(!mesh_session::is_running());
  mesh_session::Request on = make_request(mesh_session::RequestType::SET_ENABLED);
  on.enabled = true;
  assert(mesh_session::submit_request(on));
  mesh_session::process(50);
  assert(mesh_session::take_request_result(&res));
  assert(res.enabled && mesh_session::is_running());

  /* A finished result nobody collects: abandon frees the slot. */
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::CLEAR_ALERTS)));
  mesh_session::process(60);
  assert(!mesh_session::withdraw_request());   /* already ran */
  mesh_session::abandon_request();
  assert(!mesh_session::take_request_result(&res));
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::NONE)));
  mesh_session::process(70);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::BAD_REQUEST);

  /* REMOVE through the slot; the rotation it starts refuses LEAVE and
   * SET_ENABLED {false} (they would strand it), and a second REMOVE. */
  mesh_session::set_rekey_commit_handler(on_rekey_commit_abandoning);
  g_commits.clear();
  uint8_t b_pub[32], b_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  mesh_session::Request rm = make_request(mesh_session::RequestType::REMOVE);
  mesh_crypto::compute_fingerprint(x_pub, rm.fp);
  assert(mesh_session::submit_request(rm));
  mesh_session::process(1000);
  assert(mesh_session::take_request_result(&res));
  assert(res.remove == mesh_session::RemoveResult::STARTED);
  assert(std::memcmp(res.removed_pubkey, x_pub, 32) == 0);
  assert(mesh_session::rekey_in_progress());

  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::LEAVE)));
  mesh_session::process(1001);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT);
  assert(mesh_session::has_opera());
  assert(mesh_session::submit_request(off));
  mesh_session::process(1002);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT);
  assert(res.enabled && mesh_session::is_enabled());
  mesh_session::process(1000 + mesh_rekey::REKEY_TIMEOUT_MS);   /* commit */
  assert(!mesh_session::rekey_in_progress());
  assert(g_commits.size() == 1);

  assert(mesh_session::trusted_peer_count() == 0);   /* silent B dropped */

  /* REMOVE is refused while a pairing runs: the joiner would get the
   * secret the rotation is about to retire. */
  assert(mesh_session::register_trusted_peer(b_pub));
  mesh_crypto::compute_fingerprint(b_pub, rm.fp);
  assert(mesh_session::start_pairing_joiner(1100));
  assert(mesh_session::submit_request(rm));
  mesh_session::process(1101);
  assert(mesh_session::take_request_result(&res));
  assert(res.remove == mesh_session::RemoveResult::PAIRING);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(!mesh_session::rekey_in_progress());
  mesh_session::cancel_pairing();

  /* The handler gives up while its request is RUNNING — here from inside
   * the zero-survivor commit that REMOVE triggers. The request completes,
   * its result is dropped, and the slot frees itself. */
  g_commits.clear();
  g_abandon_in_commit = true;
  assert(mesh_session::submit_request(rm));
  mesh_session::process(1200);
  g_abandon_in_commit = false;
  assert(g_commits.size() == 1);                     /* it did run */
  assert(mesh_session::trusted_peer_count() == 0);
  assert(!mesh_session::take_request_result(&res));  /* result discarded */
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::CLEAR_ALERTS)));
  mesh_session::process(1201);
  assert(mesh_session::take_request_result(&res));

  /* LEAVE through the slot forgets the opera and the radio peer table:
   * the member's bound address (forgetting the member takes that one out
   * anyway) and a bare transport address no member holds, which only the
   * slot's clear_peers() removes. The member is there because only a bound
   * member is sent the LEAVE (F101), and `notified` needs one. */
  const uint8_t mac[6]   = {0x02, 0x93, 0x93, 0x93, 0x93, 0x93};
  const uint8_t other[6] = {0x02, 0x93, 0x93, 0x93, 0x93, 0x94};
  add_bound_member(mac);
  assert(mesh_transport::add_peer(other));
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::LEAVE)));
  mesh_session::process(1300);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK);
  assert(res.notified);                              /* the peer took the LEAVE */
  assert(!mesh_session::has_opera());
  assert(!mesh_transport::has_peer(mac));
  assert(!mesh_transport::has_peer(other));
  std::printf("PASS test_rest_request_slot\n");
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

/* ── F33 part 1 — the transport peer table on the device ─────────────── */

bool transport_has(const uint8_t mac[6]) { return mesh_transport::has_peer(mac); }

uint32_t dropped_no_peer() {
  mesh_transport::Stats st;
  assert(mesh_transport::get_stats(&st));
  return st.recv_dropped_no_peer;
}

/* The boot path: a trusted peer bound to its persisted radio MAC is heard
 * and reached with no add_peer by hand — the only way MACs got into the
 * table before F33 — and leaves the table when it is dropped. */
void test_bound_peer_is_heard_and_reached() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x31 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_tamper_alert_handler(on_alert_rx);

  uint8_t b_pub[32], b_priv[32], b_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0x0B};

  /* Refusals first: an untrusted fingerprint binds nothing. */
  assert(!mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(!transport_has(mac_b));
  assert(mesh_session::register_trusted_peer(b_pub));
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t zero[6]  = {0};
  const uint8_t group[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
  assert(!mesh_session::bind_peer_mac(b_fp, bcast));
  assert(!mesh_session::bind_peer_mac(b_fp, zero));
  assert(!mesh_session::bind_peer_mac(b_fp, group));

  /* Before the bind a frame from B's address is a recv_dropped_no_peer. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(b_pub, b_priv, S, 1, mesh_alert::Kind::CAMERA_TAMPER,
                               5, 11, frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_b, frame, n, -50);
  mesh_transport::process();
  assert(g_alerts_rx.empty() && dropped_no_peer() == 1);

  assert(mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(transport_has(mac_b));
  assert(mesh_session::bind_peer_mac(b_fp, mac_b));   /* idempotent */
  /* Bound but not heard this boot: not online, though its fresh transport
   * entry starts ACTIVE. */
  assert(mesh_session::online_peer_count() == 0);

  n = build_alert_frame(b_pub, b_priv, S, 2, mesh_alert::Kind::CAMERA_TAMPER, 5, 12,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_b, frame, n, -50);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1 && dropped_no_peer() == 1);
  assert(mesh_session::online_peer_count() == 1);

  /* broadcast() reaches it. */
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::ENCLOSURE_TAMPER, 4, 9, 100));
  assert(g_outs.size() == 1 && std::memcmp(g_outs[0].mac, mac_b, 6) == 0);

  /* A second peer cannot take B's address; B moving to a new address
   * takes the old one out of the table. */
  uint8_t c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  assert(mesh_session::register_trusted_peer(c_pub));
  assert(!mesh_session::bind_peer_mac(c_fp, mac_b));
  const uint8_t mac_b2[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0xB2};
  assert(mesh_session::bind_peer_mac(b_fp, mac_b2));
  assert(!transport_has(mac_b) && transport_has(mac_b2));

  /* Dropping the peer takes its address out: unregister... */
  assert(mesh_session::unregister_trusted_peer(b_fp));
  assert(!transport_has(mac_b2));
  /* ...and a verified LEAVE from a bound peer. */
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0x0C};
  assert(mesh_session::bind_peer_mac(c_fp, mac_c));
  mesh_session::set_peer_left_handler(on_peer_left);
  n = build_signed_session_frame(c_pub, c_priv, S, 1, mesh_envelope::MsgType::LEAVE_OPERA,
                                 nullptr, 0, frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -50);
  mesh_transport::process();
  assert(g_left.size() == 1);
  assert(!transport_has(mac_c));
  assert(mesh_transport::peer_count() == 0);
  std::printf("PASS test_bound_peer_is_heard_and_reached\n");
}

/* A peer the rotation forgets leaves the transport table too. */
void test_removed_peer_leaves_transport_table() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x51 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  uint8_t x_pub[32], x_priv[32], x_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0x0D};
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::bind_peer_mac(x_fp, mac_x));
  uint8_t removed[32];
  assert(mesh_session::remove_peer(x_fp, 10, removed) == mesh_session::RemoveResult::COMMITTED);
  assert(!transport_has(mac_x));
  std::printf("PASS test_removed_peer_leaves_transport_table\n");
}

/* ── F49 part 3, withdrawn — a frame from a new address moves no binding ──
 *
 * A verified frame proves who SIGNED it, not which radio sent it. The
 * envelope signs version, msg_type, opera_id, sender_fp, counter, timestamp
 * and payload — no source address, no destination — and a sender spends ONE
 * outbound counter across every destination. So a genuine frame of member
 * B's that this device has not heard yet (a broadcast it missed while off,
 * out of range or on a lost frame; a rotation frame B unicast to another
 * member) passes signature, opera_id and the strict counter from ANY
 * address. #1756 let such a frame, arriving from an address the transport
 * did not hold, move B's binding there and hand it to main.cpp to persist.
 * These pin that it no longer can: a frame from an unbound address drops
 * (recv_dropped_no_peer) before any check, as it did before #1756, B stays
 * bound where it paired, and a changed radio MAC means a re-pair (with the
 * limits test_repair_moves_a_trusted_peers_address names). */

/* The address this device last verified `fp` speaking from, if any. */
bool verified_link_mac(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN], uint8_t out[6]) {
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  const size_t n = mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS);
  for (size_t i = 0; i < n; ++i) {
    if (std::memcmp(links[i].fp, fp, mesh_crypto::FINGERPRINT_LEN) == 0 && links[i].mac_known) {
      std::memcpy(out, links[i].mac, 6);
      return true;
    }
  }
  return false;
}

void test_bound_peer_new_address_is_dropped_not_learned() {
  /* Rewritten from #1756's test_peer_new_radio_mac_is_learned_from_a_verified_frame,
   * which pinned the opposite: a fresh, verified frame from a bound peer's
   * NEW address re-bound the peer there and fired a learned-MAC callback
   * that main.cpp persisted. "Fresh and verified" turned out not to mean
   * "sent by that peer, from that address" (the block comment above; the two
   * tests below replay one from an outsider), so that path is gone. What
   * holds now: the frame drops like any frame from an unknown address, even
   * though it would verify; the peer stays bound to the address it paired
   * from; its counter is not spent, so the same frame from that address
   * still lands; and a re-pair — which binds the address the partner paired
   * from — is what moves it. */
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x47 + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(S, rx_pub, rx_priv);
  mesh_session::set_tamper_alert_handler(on_alert_rx);

  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN], tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);

  /* The address the peer paired from, bound the way boot does it. */
  const uint8_t mac_a[6] = {0x02, 0xA1, 0xA2, 0xA3, 0xA4, 0xAA};
  assert(mesh_session::bind_peer_mac(tx_fp, mac_a));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(tx_pub, tx_priv, S, /*counter=*/5,
                               mesh_alert::Kind::CAMERA_TAMPER, 6, 100,
                               frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_a, frame, n, -50);
  mesh_transport::process();
  assert(mesh_session::alerts_received() == 1);

  /* The same signer, a fresh counter, an address the transport has never
   * seen (no add_peer — that is the point). */
  const uint8_t mac_b[6] = {0x02, 0xB1, 0xB2, 0xB3, 0xB4, 0xBB};
  n = build_alert_frame(tx_pub, tx_priv, S, 6, mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 101,
                        frame, sizeof(frame));
  const uint32_t drops = dropped_no_peer();
  mesh_transport::test::inject_recv(mac_b, frame, n, -50);
  mesh_transport::process();
  /* No re-bind: the old address stays, the new one is not let in... */
  uint8_t seen[6];
  assert(transport_has(mac_a));
  assert(!transport_has(mac_b));
  assert(verified_link_mac(tx_fp, seen) && std::memcmp(seen, mac_a, 6) == 0);
  /* ...because the frame was dropped unread, not dispatched. */
  assert(mesh_session::alerts_received() == 1);
  assert(dropped_no_peer() == drops + 1);

  /* Its counter was not spent: from the bound address it lands. */
  mesh_transport::test::inject_recv(mac_a, frame, n, -50);
  mesh_transport::process();
  assert(mesh_session::alerts_received() == 2);

  /* A re-pair moves it: a completed pairing binds the address the partner
   * paired from (end_pair_contact -> bind_peer_mac), and main.cpp's
   * PairedCallback persists that one. The old address leaves the table. */
  assert(mesh_session::bind_peer_mac(tx_fp, mac_b));
  assert(transport_has(mac_b) && !transport_has(mac_a));
  n = build_alert_frame(tx_pub, tx_priv, S, 7, mesh_alert::Kind::TEMP_DRIFT, 3, 102,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_b, frame, n, -50);
  mesh_transport::process();
  assert(mesh_session::alerts_received() == 3);
  assert(verified_link_mac(tx_fp, seen) && std::memcmp(seen, mac_b, 6) == 0);
  std::printf("PASS test_bound_peer_new_address_is_dropped_not_learned\n");
}

void test_unheard_broadcast_replayed_from_a_new_address_moves_nothing() {
  /* Member B broadcast a BEACON_EVENT that this device, A, never heard; an
   * outsider E recorded it off the air and sends it to A from E's own
   * address (no spoofing: E's real MAC, which no member holds). On #1756, A
   * dispatched it, moved B's binding to E — B's real address left A's
   * transport table — and fired the learned-MAC callback main.cpp
   * persisted; A's next rotation then sent its OFFER to E alone, B never
   * answered, and the 60 s commit forgot B. */
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x93 + i);
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(S, a_pub, a_priv);
  mesh_session::set_beacon_event_handler(on_beacon_event_received);
  g_received.clear();

  uint8_t b_pub[32], b_priv[32], c_pub[32], c_priv[32], b_fp[8], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(c_pub));
  const uint8_t mac_b[6] = {0x02, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  const uint8_t mac_c[6] = {0x02, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
  const uint8_t mac_e[6] = {0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  assert(mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(mesh_session::bind_peer_mac(c_fp, mac_c));

  /* B's broadcast, counter 1, signed with B's own key. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_beacon_frame(b_pub, b_priv, S, /*counter=*/1,
                                      mesh_beacon::BeaconState::ARRIVED, "front-door",
                                      frame, sizeof(frame));
  assert(n > 0);

  /* E delivers it. */
  const uint32_t drops = dropped_no_peer();
  mesh_transport::test::inject_recv(mac_e, frame, n, -40);
  mesh_transport::process();
  /* B stays bound to its real address; E is not let in. */
  uint8_t seen[6];
  assert(transport_has(mac_b) && transport_has(mac_c));
  assert(!transport_has(mac_e));
  assert(!verified_link_mac(b_fp, seen));
  /* Dropped unread, not dispatched — and with no learned-MAC callback left
   * in the session, nothing reaches main.cpp to persist. */
  assert(g_received.empty());
  assert(dropped_no_peer() == drops + 1);

  /* The drop spent none of B's counter: the copy B itself sends lands. */
  mesh_transport::test::inject_recv(mac_b, frame, n, -40);
  mesh_transport::process();
  assert(g_received.size() == 1);
  assert(verified_link_mac(b_fp, seen) && std::memcmp(seen, mac_b, 6) == 0);

  /* A rotation A starts (removing C) reaches B: the OFFER, and its retry,
   * go to B's real address and nowhere else. */
  g_outs.clear();
  uint8_t removed[mesh_crypto::PUBKEY_LEN];
  assert(mesh_session::remove_peer(c_fp, 1000, removed) ==
         mesh_session::RemoveResult::STARTED);
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_b, 6) == 0);
  assert(g_outs[0].bytes[mesh_envelope::OFFSET_MSG_TYPE] ==
         static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_OFFER));
  g_outs.clear();
  mesh_session::process(1000 + mesh_rekey::REKEY_RETRY_MS);
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_b, 6) == 0);
  std::printf("PASS test_unheard_broadcast_replayed_from_a_new_address_moves_nothing\n");
}

void test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing() {
  /* B removes member C and sends its REKEY_OFFER; A never heard its copy,
   * and E sends it to A from E's address. On #1756, A re-bound B to E,
   * joined the rotation, and unicast its REKEY_ACCEPT — meant for the
   * initiator, B — to E. */
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xA7 + i);
  uint8_t a_pub[mesh_crypto::PUBKEY_LEN], a_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(S, a_pub, a_priv);

  uint8_t b_pub[32], b_priv[32], c_pub[32], c_priv[32];
  uint8_t a_fp[8], b_fp[8], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(c_pub));
  const uint8_t mac_b[6] = {0x02, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  const uint8_t mac_c[6] = {0x02, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
  const uint8_t mac_e[6] = {0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  assert(mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(mesh_session::bind_peer_mac(c_fp, mac_c));

  /* B's genuine OFFER (B removes C; A is the survivor), counter 2. */
  mesh_rekey::Context cb;
  mesh_rekey::context_init(cb);
  uint8_t surv[1][mesh_crypto::FINGERPRINT_LEN];
  std::memcpy(surv[0], a_fp, sizeof(a_fp));
  mesh_rekey::Action offer = mesh_rekey::start(cb, b_fp, c_fp, surv, 1, 0x5157, 0);
  assert(offer.type == mesh_rekey::ActionType::BROADCAST_OFFER);
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_signed_session_frame(b_pub, b_priv, S, /*counter=*/2,
                                              mesh_envelope::MsgType::REKEY_OFFER,
                                              offer.payload, offer.payload_len,
                                              frame, sizeof(frame));
  assert(n > 0);

  /* E delivers it. */
  g_outs.clear();
  const uint32_t drops = dropped_no_peer();
  mesh_transport::test::inject_recv(mac_e, frame, n, -40);
  mesh_transport::process();
  /* B stays bound to its real address; E is not let in. */
  uint8_t seen[6];
  assert(transport_has(mac_b) && transport_has(mac_c));
  assert(!transport_has(mac_e));
  assert(!verified_link_mac(b_fp, seen));
  /* Dropped unread: no rotation joined, C neither forgotten nor
   * deny-listed, and nothing sent — no ACCEPT to E, or to anyone. */
  assert(!mesh_session::rekey_in_progress());
  assert(mesh_session::trusted_peer_count() == 2);
  assert(!mesh_session::is_revoked(c_fp));
  assert(g_outs.empty());
  assert(dropped_no_peer() == drops + 1);

  /* The OFFER from B's own address still lands, and the ACCEPT goes back
   * to B's real address. */
  mesh_transport::test::inject_recv(mac_b, frame, n, -40);
  mesh_transport::process();
  assert(mesh_session::rekey_in_progress());
  assert(mesh_session::is_revoked(c_fp));
  assert(g_outs.size() == 1);
  assert(std::memcmp(g_outs[0].mac, mac_b, 6) == 0);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(parse_session_frame(g_outs[0].bytes, a_pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACCEPT));
  mesh_rekey::wipe(offer);
  std::printf("PASS test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing\n");
}

/* Integration-layer stand-in for main.cpp's PairedCallback: persist the
 * joiner's secret (here: set it) and register the partner. */
std::vector<uint8_t> g_paired_mac;
void on_paired_register(const uint8_t* secret, uint32_t code) {
  on_paired(secret, code);
  if (secret != nullptr) assert(mesh_session::set_opera_secret(secret));
  uint8_t peer_pub[32];
  assert(mesh_session::get_paired_peer_pubkey(peer_pub));
  assert(mesh_session::register_trusted_peer(peer_pub));
  uint8_t mac[6];
  assert(mesh_session::get_paired_peer_mac(mac));
  g_paired_mac.assign(mac, mac + 6);
}

/* The last frame the session sent to `to` (with its type byte), or empty. */
std::vector<uint8_t> last_to(const uint8_t to[6]) {
  for (size_t i = g_outs.size(); i-- > 0;) {
    if (std::memcmp(g_outs[i].mac, to, 6) == 0) return g_outs[i].bytes;
  }
  return {};
}

void feed_pure(mesh_pairing::PairingContext& ctx, const uint8_t from[6],
               const std::vector<uint8_t>& frame, uint32_t now,
               mesh_pairing::Action* out) {
  assert(!frame.empty());
  *out = mesh_pairing::receive(ctx, from, static_cast<mesh_pairing::MsgType>(frame[0]),
                               frame.data() + 1, frame.size() - 1, now);
}

std::vector<uint8_t> wire(const mesh_pairing::Action& a) {
  mesh_session::MsgType t;
  switch (a.type) {
    case mesh_pairing::ActionType::BROADCAST_DISCOVER: t = mesh_session::MsgType::PAIR_DISCOVER; break;
    case mesh_pairing::ActionType::SEND_OFFER:         t = mesh_session::MsgType::PAIR_OFFER;    break;
    case mesh_pairing::ActionType::SEND_ACCEPT:        t = mesh_session::MsgType::PAIR_ACCEPT;   break;
    case mesh_pairing::ActionType::SEND_CONFIRM:       t = mesh_session::MsgType::PAIR_CONFIRM;  break;
    case mesh_pairing::ActionType::SEND_COMPLETE:      t = mesh_session::MsgType::PAIR_COMPLETE; break;
    default: assert(false); return {};
  }
  std::vector<uint8_t> f(1 + a.payload_len);
  f[0] = static_cast<uint8_t>(t);
  std::memcpy(f.data() + 1, a.payload, a.payload_len);
  return f;
}

/* Every frame an action puts on the air, in order: for a COMPLETE, the
 * initiator's own CONFIRM in front of it (F97), then the COMPLETE. */
std::vector<std::vector<uint8_t>> wire_all(const mesh_pairing::Action& a) {
  std::vector<std::vector<uint8_t>> out;
  if (a.type == mesh_pairing::ActionType::SEND_COMPLETE) {
    assert(a.leading_confirm_present);
    std::vector<uint8_t> cf(1 + sizeof(a.leading_confirm));
    cf[0] = static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM);
    std::memcpy(cf.data() + 1, &a.leading_confirm, sizeof(a.leading_confirm));
    out.push_back(cf);
  }
  out.push_back(wire(a));
  return out;
}

void inject_all(const uint8_t from[6], const std::vector<std::vector<uint8_t>>& frames) {
  for (const auto& f : frames) {
    mesh_transport::test::inject_recv(from, f.data(), f.size(), -40);
    mesh_transport::process();
  }
}

/* A whole pairing over the air as the INITIATOR, with nothing added to the
 * transport table by hand: the joiner's frames arrive from an unknown MAC,
 * the partner's address is added for the replies, and on PAIRED the new
 * member is bound to it — its later opera frames are heard. No frame is a
 * recv_dropped_no_peer. The joiner is a pure mesh_pairing context. */
void test_pairing_over_the_air_as_initiator() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x71 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  assert(mesh_session::set_opera_secret(S));
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x01, 0x01};   /* this session */
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x01, 0x02};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);

  /* An unknown MAC's pairing frame before any pairing runs is dropped. */
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, 10);
  const std::vector<uint8_t> disc = wire(a);
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(dropped_no_peer() == 1 && g_outs.empty());

  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(dropped_no_peer() == 1);
  assert(transport_has(mac_j));                       /* the partner, for the replies */
  const std::vector<uint8_t> offer = last_to(mac_j);
  assert(!offer.empty() && offer[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_OFFER));

  feed_pure(cj, me, offer, 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const uint32_t code_j = a.confirmation_code;
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(g_code_ready == code_j);                     /* real X25519, same code */

  assert(mesh_session::confirm_pairing_code(40));
  const std::vector<uint8_t> conf_i = last_to(mac_j);
  a = mesh_pairing::confirm_code(cj, 40);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  const std::vector<uint8_t> complete = last_to(mac_j);
  assert(!complete.empty() &&
         complete[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE));
  assert(g_outs.size() >= 2 && g_outs[g_outs.size() - 2].bytes == conf_i);   /* F97: in front */
  feed_pure(cj, me, conf_i, 50, &a);
  feed_pure(cj, me, complete, 50, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);

  mesh_session::process(60);                           /* initiator's NOTIFY_PAIRED */
  assert(g_paired_fired);
  assert(g_paired_mac.size() == 6 && std::memcmp(g_paired_mac.data(), mac_j, 6) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(transport_has(mac_j));                       /* now the member's radio MAC */

  /* Its opera frames are heard. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 4,
                                     frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_j, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1);
  assert(dropped_no_peer() == 1);                      /* only the pre-pairing one */

  /* Its address is the member's: dropping the member takes it out. */
  uint8_t j_fp[8];
  mesh_crypto::compute_fingerprint(j_pub, j_fp);
  assert(mesh_session::unregister_trusted_peer(j_fp));
  assert(!transport_has(mac_j));
  std::printf("PASS test_pairing_over_the_air_as_initiator  (code=%06u)\n", code_j);
}

/* The same as the JOINER: the initiator's OFFER comes from an unknown MAC;
 * on PAIRED the initiator is bound. */
void test_pairing_over_the_air_as_joiner() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x81 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x02, 0x01};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x00, 0x02, 0x02};
  uint8_t i_pub[32], i_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 10);

  assert(mesh_session::start_pairing_joiner(20));
  const std::vector<uint8_t> disc = last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  feed_pure(ci, me, disc, 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_OFFER);
  const std::vector<uint8_t> offer = wire(a);
  mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_i));
  const std::vector<uint8_t> accept = last_to(mac_i);
  feed_pure(ci, me, accept, 40, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  assert(a.confirmation_code == mesh_session::pairing_confirmation_code());

  a = mesh_pairing::confirm_code(ci, 50);
  const std::vector<uint8_t> conf_i = wire(a);
  assert(mesh_session::confirm_pairing_code(50));
  const std::vector<uint8_t> conf_j = last_to(mac_i);
  mesh_transport::test::inject_recv(mac_i, conf_i.data(), conf_i.size(), -40);
  mesh_transport::process();
  feed_pure(ci, me, conf_j, 60, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  inject_all(mac_i, wire_all(a));                     /* its CONFIRM, then the COMPLETE */
  assert(g_paired_fired && g_paired_with_secret);
  assert(std::memcmp(g_paired_secret, S, 32) == 0);
  assert(g_paired_mac.size() == 6 && std::memcmp(g_paired_mac.data(), mac_i, 6) == 0);
  assert(transport_has(mac_i));
  assert(dropped_no_peer() == 0);
  std::printf("PASS test_pairing_over_the_air_as_joiner\n");
}

/* F97 through the session, as the INITIATOR, the joiner's owner confirming
 * first: its CONFIRM arrives before this device's owner confirms. The
 * session sends nothing then; the owner's confirm, through the REST slot,
 * sends exactly two frames to the joiner, this side's CONFIRM and then the
 * COMPLETE, and the same process() reports PAIRED. On the code before F97
 * the CONFIRM was dropped, the confirm sent a CONFIRM alone, and both sides
 * timed out. */
void test_pairing_over_the_air_joiner_confirms_first() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x97 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  assert(mesh_session::set_opera_secret(S));
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x31};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x32};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, 10);
  const std::vector<uint8_t> disc = wire(a);

  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  feed_pure(cj, me, last_to(mac_j), 30, &a);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);

  /* The joiner's owner confirms first. */
  a = mesh_pairing::confirm_code(cj, 30);
  const std::vector<uint8_t> conf_j = wire(a);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  mesh_session::process(35);
  assert(g_outs.empty() && !g_failed_fired && !g_paired_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);

  /* This owner confirms, through the REST slot. */
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CONFIRM)));
  mesh_session::process(40);
  mesh_session::RequestResult res;
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK);
  assert(g_outs.size() == 2);
  assert(std::memcmp(g_outs[0].mac, mac_j, 6) == 0);
  assert(std::memcmp(g_outs[1].mac, mac_j, 6) == 0);
  assert(g_outs[0].bytes == conf_j);                  /* the hash is the same both ways */
  assert(g_outs[1].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE));
  assert(g_paired_fired);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(transport_has(mac_j));

  feed_pure(cj, me, g_outs[0].bytes, 45, &a);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(cj.state == mesh_pairing::State::AWAITING_COMPLETE);
  feed_pure(cj, me, g_outs[1].bytes, 50, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  uint8_t got[32];
  assert(mesh_pairing::consume_opera_secret(cj, got));
  assert(std::memcmp(got, S, sizeof(S)) == 0);
  std::printf("PASS test_pairing_over_the_air_joiner_confirms_first\n");
}

/* F97 through the session, as the JOINER, the initiator's owner confirming
 * first: its CONFIRM reaches this device before its owner confirms. The
 * session keeps waiting for its owner, sends its CONFIRM at the confirm,
 * and takes the COMPLETE that answers it. On the code before F97 that
 * COMPLETE was dropped (the joiner waited for the initiator's CONFIRM,
 * already dropped), so the initiator held a member that never joined. */
void test_pairing_over_the_air_initiator_confirms_first() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xA7 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x41};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x42};
  uint8_t i_pub[32], i_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 10);

  assert(mesh_session::start_pairing_joiner(20));
  feed_pure(ci, me, last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), 30, &a);
  const std::vector<uint8_t> offer = wire(a);
  mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
  mesh_transport::process();
  feed_pure(ci, me, last_to(mac_i), 40, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);

  /* The initiator's owner confirms first. */
  a = mesh_pairing::confirm_code(ci, 50);
  const std::vector<uint8_t> conf_i = wire(a);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_i, conf_i.data(), conf_i.size(), -40);
  mesh_transport::process();
  assert(g_outs.empty() && !g_failed_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);

  assert(mesh_session::confirm_pairing_code(60));
  assert(g_outs.size() == 1);
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM));
  feed_pure(ci, me, g_outs[0].bytes, 70, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  inject_all(mac_i, wire_all(a));                     /* its CONFIRM again, then the COMPLETE */
  assert(g_paired_fired && g_paired_with_secret);
  assert(std::memcmp(g_paired_secret, S, 32) == 0);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(transport_has(mac_i));
  mesh_session::process(80 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(!g_failed_fired);
  std::printf("PASS test_pairing_over_the_air_initiator_confirms_first\n");
}

/* A joiner on firmware before F97 (c6a305b's rules), modeled on a pure
 * joiner context: it reads a CONFIRM only once its own owner confirmed
 * (AWAITING_CONFIRM_PEER) and takes a COMPLETE only after such a CONFIRM
 * (AWAITING_COMPLETE); anything else it drops unread. */
void feed_pre_f97_joiner(mesh_pairing::PairingContext& cj, const uint8_t from[6],
                         const std::vector<uint8_t>& frame, uint32_t now,
                         mesh_pairing::Action* out) {
  assert(!frame.empty());
  if (frame[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM) &&
      cj.state != mesh_pairing::State::AWAITING_CONFIRM_PEER) {
    *out = mesh_pairing::Action{};
    return;
  }
  if (frame[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE) &&
      cj.state != mesh_pairing::State::AWAITING_COMPLETE) {
    *out = mesh_pairing::Action{};
    return;
  }
  feed_pure(cj, from, frame, now, out);
}

/* F97 against older firmware: this session as the INITIATOR pairs a joiner
 * on the pre-F97 rules in either order, every frame it sends delivered as
 * sent. It works because the COMPLETE goes out with this side's CONFIRM in
 * front of it. With the COMPLETE sent alone (the first F97 change), the
 * joiner dropped it in both orders while this side reported PAIRED, and
 * main.cpp registered, bound and stored a member that never joined. */
void test_pairing_over_the_air_with_a_pre_f97_joiner() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    uint8_t S[32];
    for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xB7 + i);
    uint8_t pub[32], priv[32];
    stand_up_session(nullptr, pub, priv);
    mesh_session::set_paired_callback(on_paired_register);
    assert(mesh_session::set_opera_secret(S));
    g_paired_mac.clear();

    const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x51};
    const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x52};
    uint8_t j_pub[32], j_priv[32];
    assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
    mesh_pairing::PairingContext cj;
    mesh_pairing::context_init(cj);
    mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, 10);
    const std::vector<uint8_t> disc = wire(a);

    assert(mesh_session::start_pairing_initiator(S, "Home", 20));
    mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
    mesh_transport::process();
    feed_pure(cj, me, last_to(mac_j), 25, &a);
    const std::vector<uint8_t> accept = wire(a);
    mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
    mesh_transport::process();
    assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);

    if (joiner_first) {
      a = mesh_pairing::confirm_code(cj, 30);
      const std::vector<uint8_t> conf_j = wire(a);
      mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
      mesh_transport::process();
      g_outs.clear();
      assert(mesh_session::confirm_pairing_code(40));
    } else {
      g_outs.clear();
      assert(mesh_session::confirm_pairing_code(30));
      assert(g_outs.size() == 1);
      feed_pre_f97_joiner(cj, me, g_outs[0].bytes, 31, &a);   /* dropped unread */
      assert(a.type == mesh_pairing::ActionType::NONE);
      assert(cj.state == mesh_pairing::State::AWAITING_CONFIRM);
      a = mesh_pairing::confirm_code(cj, 35);
      const std::vector<uint8_t> conf_j = wire(a);
      g_outs.clear();
      mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
      mesh_transport::process();
    }
    /* Every frame this session sent the joiner since, in order. */
    size_t to_j = 0;
    a = mesh_pairing::Action{};
    for (const OutFrame& f : g_outs) {
      if (std::memcmp(f.mac, mac_j, 6) != 0) continue;
      ++to_j;
      feed_pre_f97_joiner(cj, me, f.bytes, 50, &a);
    }
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);   /* not dropped */
    assert(to_j == 2);                                           /* CONFIRM, COMPLETE */
    uint8_t got[32];
    assert(mesh_pairing::consume_opera_secret(cj, got));
    assert(std::memcmp(got, S, sizeof(S)) == 0);
    mesh_session::process(60);
    assert(g_paired_fired && mesh_session::trusted_peer_count() == 1);
  }
  std::printf("PASS test_pairing_over_the_air_with_a_pre_f97_joiner  (both orders)\n");
}

/* An initiator on firmware before F97 (c6a305b's rules), modeled on a pure
 * initiator context: it reads a CONFIRM only once its own owner confirmed
 * (AWAITING_CONFIRM_PEER) and drops one that comes earlier unread; its
 * COMPLETE goes alone (the old Action had no leading CONFIRM). */
void feed_pre_f97_initiator(mesh_pairing::PairingContext& ci, const uint8_t from[6],
                            const std::vector<uint8_t>& frame, uint32_t now,
                            mesh_pairing::Action* out) {
  assert(!frame.empty());
  if (frame[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM) &&
      ci.state != mesh_pairing::State::AWAITING_CONFIRM_PEER) {
    *out = mesh_pairing::Action{};
    return;
  }
  feed_pure(ci, from, frame, now, out);
}

/* F117 through the session, as the JOINER, on the main loop's clock.
 *  • A pre-F97 initiator, the joiner's owner first: the joiner's CONFIRM is
 *    dropped; the initiator's owner confirms and its CONFIRM reaches the
 *    session at loop time T; process() re-sends nothing before
 *    T + CONFIRM_RESEND_FIRST_MS and then exactly one copy of the joiner's
 *    CONFIRM, to the initiator, which answers with the COMPLETE alone: PAIRED
 *    with the secret. On the code before F117 both sides timed out.
 *  • The same initiator, its owner first: PAIRED on the first CONFIRM, and
 *    nothing is re-sent.
 *  • An updated initiator whose COMPLETE lands one loop pass after its
 *    CONFIRM (two frames, two passes): nothing is re-sent in between.
 * The session's receive path stamps the frame with the last process() time;
 * it used to pass 0, which would make the first copy due at once (fails
 * with that restored, and with CONFIRM_RESEND_FIRST_MS taken as 0). */
void test_pairing_over_the_air_with_a_pre_f97_initiator() {
  for (int c = 0; c < 3; ++c) {
    const bool pre_f97 = c < 2, joiner_first = c != 1;
    uint8_t S[32];
    for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xC7 + i);
    uint8_t pub[32], priv[32];
    stand_up_session(nullptr, pub, priv);
    mesh_session::set_paired_callback(on_paired_register);
    const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x01, 0x17, 0x01};
    const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x01, 0x17, 0x02};
    uint8_t i_pub[32], i_priv[32];
    assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
    mesh_pairing::PairingContext ci;
    mesh_pairing::context_init(ci);
    const uint32_t T0 = 100000;   /* far from 0: a frame stamped 0 would be long overdue */
    mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", T0);
    mesh_session::process(T0);
    assert(mesh_session::start_pairing_joiner(T0));
    feed_pure(ci, me, last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), T0, &a);
    const std::vector<uint8_t> offer = wire(a);
    mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
    mesh_transport::process();
    feed_pure(ci, me, last_to(mac_i), T0, &a);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
    auto to_i = [&]() {
      size_t n = 0;
      for (const auto& o : g_outs) n += std::memcmp(o.mac, mac_i, 6) == 0;
      return n;
    };

    if (pre_f97 && joiner_first) {
      g_outs.clear();
      assert(mesh_session::confirm_pairing_code(T0 + 10));
      const std::vector<uint8_t> conf_j = last_to(mac_i);
      feed_pre_f97_initiator(ci, me, conf_j, T0 + 10, &a);
      assert(a.type == mesh_pairing::ActionType::NONE);        /* dropped unread */
      a = mesh_pairing::confirm_code(ci, T0 + 5000);
      const std::vector<uint8_t> conf_i = wire(a);
      const uint32_t T = T0 + 5000;
      mesh_session::process(T);
      mesh_transport::test::inject_recv(mac_i, conf_i.data(), conf_i.size(), -40);
      mesh_transport::process();
      g_outs.clear();
      for (uint32_t t = T; t < T + mesh_pairing::CONFIRM_RESEND_FIRST_MS; t += 100) {
        mesh_session::process(t);
      }
      mesh_session::process(T + mesh_pairing::CONFIRM_RESEND_FIRST_MS - 1);
      assert(to_i() == 0);
      mesh_session::process(T + mesh_pairing::CONFIRM_RESEND_FIRST_MS);
      assert(g_outs.size() == 1 && to_i() == 1);
      assert(g_outs[0].bytes == conf_j);                         /* the same CONFIRM */
      feed_pre_f97_initiator(ci, me, g_outs[0].bytes, T + 1100, &a);
      assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
      const std::vector<uint8_t> complete = wire(a);             /* alone */
      mesh_transport::test::inject_recv(mac_i, complete.data(), complete.size(), -40);
      mesh_transport::process();
    } else if (pre_f97) {
      a = mesh_pairing::confirm_code(ci, T0 + 10);
      const std::vector<uint8_t> conf_i = wire(a);
      mesh_transport::test::inject_recv(mac_i, conf_i.data(), conf_i.size(), -40);
      mesh_transport::process();
      g_outs.clear();
      assert(mesh_session::confirm_pairing_code(T0 + 20));
      feed_pre_f97_initiator(ci, me, last_to(mac_i), T0 + 20, &a);
      assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
      const std::vector<uint8_t> complete = wire(a);
      mesh_transport::test::inject_recv(mac_i, complete.data(), complete.size(), -40);
      mesh_transport::process();
    } else {
      /* Updated initiator, joiner's owner first; its two frames arrive in
       * two loop passes. */
      g_outs.clear();
      assert(mesh_session::confirm_pairing_code(T0 + 10));
      feed_pure(ci, me, last_to(mac_i), T0 + 10, &a);
      assert(a.type == mesh_pairing::ActionType::NONE && ci.peer_confirmed);
      a = mesh_pairing::confirm_code(ci, T0 + 20);
      const std::vector<std::vector<uint8_t>> frames = wire_all(a);
      assert(frames.size() == 2);
      g_outs.clear();
      mesh_transport::test::inject_recv(mac_i, frames[0].data(), frames[0].size(), -40);
      mesh_transport::process();
      mesh_session::process(T0 + 30);
      mesh_session::process(T0 + 500);
      assert(to_i() == 0);
      mesh_transport::test::inject_recv(mac_i, frames[1].data(), frames[1].size(), -40);
      mesh_transport::process();
    }
    assert(g_paired_fired && g_paired_with_secret);
    assert(std::memcmp(g_paired_secret, S, 32) == 0);
    assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
    const size_t sent = g_outs.size();
    for (uint32_t t = T0 + 20000; t < T0 + 40000; t += 500) mesh_session::process(t);
    assert(g_outs.size() == sent);                               /* nothing more */
  }
  std::printf("PASS test_pairing_over_the_air_with_a_pre_f97_initiator"
              "  (both orders; an updated one a pass apart)\n");
}

/* A pairing that ends without a member takes the partner's address out of
 * the table again; an unknown MAC's non-pairing frame is never taken. */
void test_failed_pairing_removes_partner_address() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x91 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x03, 0x02};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  const std::vector<uint8_t> disc = wire(mesh_pairing::start_joiner(cj, j_pub, j_priv, 10));

  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  /* An opera frame from an unknown MAC is not taken, even mid-pairing. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 4,
                                     frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_j, frame, n, -40);
  mesh_transport::process();
  assert(dropped_no_peer() == 1);

  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_j));
  mesh_session::cancel_pairing();
  assert(g_failed_fired);
  assert(!transport_has(mac_j));

  /* The 5-minute timeout does the same — and a finished pairing does not
   * block the next one (it used to: nothing reset the context to IDLE). */
  g_failed_fired = false;
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  assert(mesh_session::start_pairing_initiator(S, "Home", 1000));
  mesh_pairing::context_init(cj);
  const std::vector<uint8_t> disc2 = wire(mesh_pairing::start_joiner(cj, j_pub, j_priv, 1000));
  mesh_transport::test::inject_recv(mac_j, disc2.data(), disc2.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_j));
  mesh_session::process(1000 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(g_failed_fired);
  assert(!transport_has(mac_j));
  assert(mesh_session::start_pairing_joiner(2000 + mesh_pairing::PAIRING_TIMEOUT_MS));
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
  mesh_session::cancel_pairing();
  std::printf("PASS test_failed_pairing_removes_partner_address\n");
}

/* main.cpp's PairedCallback as it treats a device it ALREADY trusts:
 * register_trusted_peer refuses the duplicate (main.cpp logs it and goes
 * on), and the partner's address is the one it persists (save_peer_mac). */
void on_paired_known_peer(const uint8_t* secret, uint32_t code) {
  on_paired(secret, code);
  if (secret != nullptr) assert(mesh_session::set_opera_secret(secret));
  uint8_t peer_pub[32];
  assert(mesh_session::get_paired_peer_pubkey(peer_pub));
  assert(!mesh_session::register_trusted_peer(peer_pub));   /* already trusted */
  uint8_t mac[6];
  assert(mesh_session::get_paired_peer_mac(mac));
  g_paired_mac.assign(mac, mac + 6);
}

/* With F49 part 3 withdrawn, a re-pair is how a trusted peer whose radio
 * MAC changed is heard again. J is trusted and bound to the address it
 * first paired from, and now transmits from a new one: its opera frames
 * from there drop, unread; a pairing run from there binds the new address,
 * takes the old one out of the table, and hands main.cpp the address to
 * persist.
 *
 * This pins the mechanism, not that a re-pair is safe. The pairing binds
 * whatever long-term key the DISCOVER (here) or OFFER carried: the 6-digit
 * code and the CONFIRM hash cover only the ephemeral X25519 exchange. So an
 * outsider relaying an owner-run pairing between two devices, from its own
 * address, gets matching codes on both screens while claiming a trusted
 * member's key, and this same path then re-binds that member to the
 * outsider's radio and main.cpp persists it. Pre-existing (the same before
 * #1756); open, and closing it needs a wire change. Nor does a re-pair
 * start with eight members bound: the transport table has no slot for J's
 * new address, so the OFFER cannot be sent. */
void test_repair_moves_a_trusted_peers_address() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xB3 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_paired_callback(on_paired_known_peer);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  g_paired_mac.clear();

  const uint8_t me[6]      = {0x24, 0x0A, 0xC4, 0x00, 0x04, 0x01};   /* this session */
  const uint8_t mac_old[6] = {0x24, 0x0A, 0xC4, 0x00, 0x04, 0x02};
  const uint8_t mac_new[6] = {0x24, 0x0A, 0xC4, 0x00, 0x04, 0x03};
  uint8_t j_pub[32], j_priv[32], j_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);
  assert(mesh_session::register_trusted_peer(j_pub));
  assert(mesh_session::bind_peer_mac(j_fp, mac_old));

  /* J's opera frame from its new address: dropped, the binding unmoved. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 4,
                                     frame, sizeof(frame));
  const uint32_t drops = dropped_no_peer();
  mesh_transport::test::inject_recv(mac_new, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.empty() && dropped_no_peer() == drops + 1);
  assert(transport_has(mac_old) && !transport_has(mac_new));

  /* The owner re-pairs J; J runs it from its new address. */
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, 10);
  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  const std::vector<uint8_t> disc = wire(a);
  mesh_transport::test::inject_recv(mac_new, disc.data(), disc.size(), -40);
  mesh_transport::process();
  const std::vector<uint8_t> offer = last_to(mac_new);
  assert(!offer.empty() && offer[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_OFFER));
  feed_pure(cj, me, offer, 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_new, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(mesh_session::confirm_pairing_code(40));
  const std::vector<uint8_t> conf_i = last_to(mac_new);
  a = mesh_pairing::confirm_code(cj, 40);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_new, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  const std::vector<uint8_t> complete = last_to(mac_new);
  feed_pure(cj, me, conf_i, 50, &a);
  feed_pure(cj, me, complete, 50, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  mesh_session::process(60);

  /* Re-bound to the address it paired from; the old one is out; still one
   * member; and main.cpp is handed the new address to persist. */
  assert(g_paired_fired);
  assert(g_paired_mac.size() == 6 && std::memcmp(g_paired_mac.data(), mac_new, 6) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(transport_has(mac_new) && !transport_has(mac_old));
  /* The frame that was dropped spent no counter: from here it lands. */
  mesh_transport::test::inject_recv(mac_new, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1);
  std::printf("PASS test_repair_moves_a_trusted_peers_address\n");
}

/* ── F102 — a pairing's address is persisted only once the session bound it ──
 *
 * dispatch_action's NOTIFY_PAIRED runs the PairedCallback, then binds the
 * new member to the address it paired from (end_pair_contact →
 * bind_peer_mac). Until F102 main.cpp persisted that address from the
 * PairedCallback (register_paired_peer → save_peer_mac), before the bind,
 * and peer_mac_blob::upsert took an address another fingerprint held. So a
 * re-pair the session refused to bind — member J presenting its key from
 * member C's address, which F69's unauthenticated re-pair lets a relay do —
 * was still written as J's address, and the next boot, which binds
 * peer_macs in blob order, gave C's address to J and refused C's own: C was
 * not heard at all. Now the session reports the bind through the
 * PairedPeerBoundCallback, main.cpp persists from there and only when it
 * took, and upsert refuses the address too. */

/* A stand-in for main.cpp's mesh wiring, on a fake NVS: the PairedCallback
 * persists the pubkey and registers it (register_paired_peer, and the
 * joiner's secret), and the bound callback persists the address only when
 * bound (on_mesh_paired_peer_bound), through the real blob helper. */
struct BoundCall {
  uint8_t fp[8];
  uint8_t mac[6];
  bool    bound;
  bool    after_paired_cb;
};
std::vector<BoundCall>             g_bound_calls;
std::vector<std::vector<uint8_t>>  g_nvs_pubs;
uint8_t                            g_nvs_macs[mesh_state::PEER_MACS_BLOB_MAX];
size_t                             g_nvs_macs_len = 0;
bool                               g_in_paired_cb_done = false;

void main_like_paired(const uint8_t* secret, uint32_t code) {
  on_paired(secret, code);
  if (secret != nullptr) assert(mesh_session::set_opera_secret(secret));
  uint8_t pub[32];
  assert(mesh_session::get_paired_peer_pubkey(pub));
  bool stored = false;
  for (const auto& p : g_nvs_pubs) stored |= std::memcmp(p.data(), pub, 32) == 0;
  if (!stored) g_nvs_pubs.emplace_back(pub, pub + 32);
  (void)mesh_session::register_trusted_peer(pub);   /* refused when already trusted */
  g_in_paired_cb_done = true;
}

void main_like_bound(const uint8_t fp[8], const uint8_t mac[6], bool bound) {
  BoundCall c;
  std::memcpy(c.fp, fp, 8);
  std::memcpy(c.mac, mac, 6);
  c.bound = bound;
  c.after_paired_cb = g_in_paired_cb_done;
  g_bound_calls.push_back(c);
  if (bound) (void)mesh_state::peer_mac_blob::upsert(g_nvs_macs, &g_nvs_macs_len, fp, mac);
}

void reset_fake_main_nvs() {
  g_bound_calls.clear();
  g_nvs_pubs.clear();
  std::memset(g_nvs_macs, 0, sizeof(g_nvs_macs));
  g_nvs_macs_len = 0;
}

/* Run one whole pairing with this session as the INITIATOR and a pure
 * joiner holding (j_pub, j_priv) at mac_j, the joiner's owner confirming
 * first. Returns true once the session's NOTIFY_PAIRED has fired; false
 * when the session refused the partner at its owner's confirm (F118: a
 * member it could not hold), in which case nothing was sealed or sent to
 * the joiner and neither paired callback ran. */
bool pair_from(const uint8_t S[32], const uint8_t me[6], const uint8_t mac_j[6],
               const uint8_t j_pub[32], const uint8_t j_priv[32], uint32_t t) {
  g_in_paired_cb_done = false;
  g_paired_fired = false;
  g_failed_fired = false;
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, t);
  assert(mesh_session::start_pairing_initiator(S, "Home", t));
  const std::vector<uint8_t> disc = wire(a);
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  feed_pure(cj, me, last_to(mac_j), t + 1, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  a = mesh_pairing::confirm_code(cj, t + 2);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  const size_t frames_before = g_outs.size();
  const size_t bound_before  = g_bound_calls.size();
  if (!mesh_session::confirm_pairing_code(t + 3)) {
    assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::PARTNER_REFUSED);
    assert(g_failed_fired && g_failed_why == mesh_pairing::FailReason::PARTNER_REFUSED);
    uint8_t j_fp[8];
    mesh_crypto::compute_fingerprint(j_pub, j_fp);
    assert(g_failed_fp.size() == 8 && std::memcmp(g_failed_fp.data(), j_fp, 8) == 0);
    assert(g_outs.size() == frames_before);          /* no CONFIRM, no COMPLETE */
    mesh_session::process(t + 4);
    assert(!g_paired_fired && g_bound_calls.size() == bound_before);
    return false;
  }
  mesh_session::process(t + 4);
  assert(g_paired_fired);
  feed_pure(cj, me, last_to(mac_j), t + 5, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  return true;
}

/* main.cpp's PairedCallback when register_trusted_peer refuses the partner
 * for a reason the session's gate cannot foresee (F118 asks
 * can_hold_partner before the pairing completes, so the re-pair from
 * another member's address and the new member for a full table no longer
 * get this far): here it registers nothing. */
void main_like_paired_unregistered(const uint8_t* secret, uint32_t code) {
  on_paired(secret, code);
  g_in_paired_cb_done = true;
}

/* The bound callback fires once per completed pairing, after the
 * PairedCallback, with the outcome of the bind: true for a new member at a
 * free address, and false for a member the PairedCallback did not register
 * (the partner's address then leaves with the pairing). Until F118 it also
 * reported false for a re-pair from an address another member holds and
 * for a new member when the trusted table was full; those pairings now
 * fail at the owner's confirm (F118), so neither paired callback runs. */
void test_paired_peer_bound_reports_the_bind() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x02 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x02, 0x00, 0x01};
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x02, 0x00, 0x0A};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x02, 0x00, 0x0C};
  uint8_t j_pub[32], j_priv[32], j_fp[8], c_pub[32], c_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);

  /* A new member at a free address: bound. */
  assert(pair_from(S, me, mac_x, j_pub, j_priv, 100));
  assert(g_bound_calls.size() == 1);
  assert(g_bound_calls[0].bound && g_bound_calls[0].after_paired_cb);
  assert(std::memcmp(g_bound_calls[0].fp, j_fp, 8) == 0);
  assert(std::memcmp(g_bound_calls[0].mac, mac_x, 6) == 0);
  assert(pair_from(S, me, mac_c, c_pub, c_priv, 200));
  assert(g_bound_calls.size() == 2 && g_bound_calls[1].bound);

  /* J re-pairs from C's address: refused at the confirm since F118, so no
   * bound callback (it used to report false here). */
  assert(!pair_from(S, me, mac_c, j_pub, j_priv, 300));
  assert(g_bound_calls.size() == 2);
  /* The session kept J at X and C at its own address. */
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(c_pub, c_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                               frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);
  n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 2,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_x, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 2);

  /* A member the PairedCallback did not register: the bind has no member
   * to bind, and reports false. */
  mesh_session::set_paired_callback(main_like_paired_unregistered);
  const uint8_t mac_n[6] = {0x24, 0x0A, 0xC4, 0x02, 0x00, 0x0D};
  uint8_t n_pub[32], n_priv[32], n_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(n_pub, n_priv));
  mesh_crypto::compute_fingerprint(n_pub, n_fp);
  assert(pair_from(S, me, mac_n, n_pub, n_priv, 400));
  assert(g_bound_calls.size() == 3);
  assert(!g_bound_calls[2].bound && g_bound_calls[2].after_paired_cb);
  assert(std::memcmp(g_bound_calls[2].fp, n_fp, 8) == 0);
  assert(std::memcmp(g_bound_calls[2].mac, mac_n, 6) == 0);
  assert(!transport_has(mac_n));   /* the partner's address left with the pairing */
  std::printf("PASS test_paired_peer_bound_reports_the_bind\n");
}

/* The F102 probe, end to end with main.cpp's wiring on a fake NVS: J paired
 * at X, C at its own address, then J re-paired from C's address. After a
 * reboot that restores the pubkeys and binds the stored addresses in blob
 * order, as main.cpp's setup does, C is heard from its own address and J
 * from X; J's frames from C's address are not taken. Before F102 NVS
 * recorded J at C's address, J took it at the reboot, and C was not heard
 * at all. Since F118 the re-pair from C's address fails at the owner's
 * confirm, so nothing reaches the callbacks at all; the stored state and
 * the reboot are the same. */
void test_refused_repair_bind_is_not_persisted_across_reboot() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x12 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x02, 0x10, 0x01};
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x02, 0x10, 0x0A};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x02, 0x10, 0x0C};
  uint8_t j_pub[32], j_priv[32], j_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  assert(pair_from(S, me, mac_x, j_pub, j_priv, 100));   /* J first: its entry is first in the blob */
  assert(pair_from(S, me, mac_c, c_pub, c_priv, 200));
  assert(!pair_from(S, me, mac_c, j_pub, j_priv, 300));  /* the re-pair from C's address */

  mesh_state::PeerMac stored[mesh_state::MAX_TRUSTED_PEERS];
  size_t n_stored = 0;
  assert(mesh_state::peer_mac_blob::decode(g_nvs_macs, g_nvs_macs_len, stored,
                                           mesh_state::MAX_TRUSTED_PEERS, &n_stored));
  assert(n_stored == 2);
  assert(std::memcmp(stored[0].fingerprint, j_fp, 8) == 0 && std::memcmp(stored[0].mac, mac_x, 6) == 0);
  assert(std::memcmp(stored[1].fingerprint, c_fp, 8) == 0 && std::memcmp(stored[1].mac, mac_c, 6) == 0);

  /* Reboot as main.cpp's setup does: same device keys, the secret, every
   * stored pubkey registered, then every stored address bound in order. */
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv) && mesh_session::start());
  assert(mesh_session::set_opera_secret(S));
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  g_alerts_rx.clear();
  for (const auto& p : g_nvs_pubs) assert(mesh_session::register_trusted_peer(p.data()));
  size_t bound = 0;
  for (size_t i = 0; i < n_stored; ++i) {
    bound += mesh_session::bind_peer_mac(stored[i].fingerprint, stored[i].mac) ? 1 : 0;
  }
  assert(bound == 2);

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(c_pub, c_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                               frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1 && std::memcmp(g_alerts_rx[0].fp, c_fp, 8) == 0);
  n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 2,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);   /* J, from C's address */
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1);
  mesh_transport::test::inject_recv(mac_x, frame, n, -40);   /* J, from its own */
  mesh_transport::process();
  assert(g_alerts_rx.size() == 2 && std::memcmp(g_alerts_rx[1].fp, j_fp, 8) == 0);
  std::printf("PASS test_refused_repair_bind_is_not_persisted_across_reboot\n");
}

/* The other side of F102: a re-pair the session DOES bind is persisted.
 * J paired at X, then re-paired from a free address Z (a swapped radio
 * module): the bound callback reports (J, Z, true) and NVS holds J at Z,
 * one entry, J's own, updated in place. A second re-pair from Z (the
 * address it already holds) reports true and changes nothing. After a
 * reboot binding the stored addresses, J is heard from Z and not from X.
 * Fails if a successful re-pair of an already-trusted member reads as
 * refused (main.cpp would then warn and keep X, and J would go unheard
 * after the reboot). */
void test_successful_repair_is_persisted_across_reboot() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x22 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x02, 0x20, 0x01};
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x02, 0x20, 0x0A};
  const uint8_t mac_z[6] = {0x24, 0x0A, 0xC4, 0x02, 0x20, 0x0F};
  uint8_t j_pub[32], j_priv[32], j_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);

  assert(pair_from(S, me, mac_x, j_pub, j_priv, 100));
  assert(g_bound_calls.size() == 1 && g_bound_calls[0].bound);

  /* The re-pair from the free address Z: bound, reported, persisted. */
  assert(pair_from(S, me, mac_z, j_pub, j_priv, 200));
  assert(g_bound_calls.size() == 2);
  assert(g_bound_calls[1].bound && g_bound_calls[1].after_paired_cb);
  assert(std::memcmp(g_bound_calls[1].fp, j_fp, 8) == 0);
  assert(std::memcmp(g_bound_calls[1].mac, mac_z, 6) == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(transport_has(mac_z) && !transport_has(mac_x));

  /* Again from Z, the address it holds: still bound, nothing moves. */
  assert(pair_from(S, me, mac_z, j_pub, j_priv, 300));
  assert(g_bound_calls.size() == 3 && g_bound_calls[2].bound);

  mesh_state::PeerMac stored[mesh_state::MAX_TRUSTED_PEERS];
  size_t n_stored = 0;
  assert(mesh_state::peer_mac_blob::decode(g_nvs_macs, g_nvs_macs_len, stored,
                                           mesh_state::MAX_TRUSTED_PEERS, &n_stored));
  assert(n_stored == 1);
  assert(std::memcmp(stored[0].fingerprint, j_fp, 8) == 0);
  assert(std::memcmp(stored[0].mac, mac_z, 6) == 0);

  /* Reboot as main.cpp's setup does (see the test above). */
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv) && mesh_session::start());
  assert(mesh_session::set_opera_secret(S));
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  g_alerts_rx.clear();
  for (const auto& p : g_nvs_pubs) assert(mesh_session::register_trusted_peer(p.data()));
  for (size_t i = 0; i < n_stored; ++i) {
    assert(mesh_session::bind_peer_mac(stored[i].fingerprint, stored[i].mac));
  }
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                                     frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_x, frame, n, -40);   /* its old address */
  mesh_transport::process();
  assert(g_alerts_rx.empty());
  mesh_transport::test::inject_recv(mac_z, frame, n, -40);   /* its new one */
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1 && std::memcmp(g_alerts_rx[0].fp, j_fp, 8) == 0);
  std::printf("PASS test_successful_repair_is_persisted_across_reboot\n");
}

/* ── F119, F120 — the boot restore of the stored addresses ─────────────────
 *
 * main.cpp's setup registers every stored pubkey, then hands the stored
 * peer_macs entries to restore_peer_macs and drops from NVS the ones it
 * calls UNTRUSTED or SHARED — only when the pubkey list was read (a failed
 * read registers nobody, and every entry would look untrusted). Until F119
 * and F120 it bound each entry in blob order and dropped nothing. */

/* That restore, on the fake NVS: a reboot with the same device keys and
 * opera secret. Returns the verdicts. */
std::vector<mesh_session::StoredMacVerdict> main_like_boot(const uint8_t pub[32],
                                                           const uint8_t priv[32],
                                                           const uint8_t S[32],
                                                           bool peers_loaded = true,
                                                           size_t* bound_out = nullptr) {
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv) && mesh_session::start());
  assert(mesh_session::set_opera_secret(S));
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  g_alerts_rx.clear();
  if (peers_loaded) {
    for (const auto& p : g_nvs_pubs) (void)mesh_session::register_trusted_peer(p.data());
  }
  mesh_state::PeerMac stored[mesh_state::MAX_TRUSTED_PEERS];
  size_t n = 0;
  assert(mesh_state::peer_mac_blob::decode(g_nvs_macs, g_nvs_macs_len, stored,
                                           mesh_state::MAX_TRUSTED_PEERS, &n));
  uint8_t fps[mesh_state::MAX_TRUSTED_PEERS][8];
  uint8_t macs[mesh_state::MAX_TRUSTED_PEERS][6];
  for (size_t i = 0; i < n; ++i) {
    std::memcpy(fps[i], stored[i].fingerprint, 8);
    std::memcpy(macs[i], stored[i].mac, 6);
  }
  std::vector<mesh_session::StoredMacVerdict> v(n);
  const size_t bound = mesh_session::restore_peer_macs(fps, macs, n, v.data());
  if (bound_out != nullptr) *bound_out = bound;
  /* main.cpp's drop loop, line for line (the scripts/tests pin holds it). */
  for (size_t i = 0; i < n; ++i) {
    if (!mesh_session::stored_mac_must_drop(v[i], peers_loaded)) continue;
    assert(mesh_state::peer_mac_blob::remove(g_nvs_macs, &g_nvs_macs_len, fps[i]));
  }
  return v;
}

/* The drop decision itself, every verdict × peers_loaded: a stored entry is
 * dropped exactly when it is SHARED or UNTRUSTED and the pubkey list was
 * read. main.cpp's boot loop asks this and nothing else, so an inverted or
 * widened drop shows here, not only on a bench after a reboot. */
void test_stored_mac_must_drop_truth_table() {
  using V = mesh_session::StoredMacVerdict;
  assert(!mesh_session::stored_mac_must_drop(V::BOUND, true));
  assert(!mesh_session::stored_mac_must_drop(V::REFUSED, true));
  assert(mesh_session::stored_mac_must_drop(V::UNTRUSTED, true));
  assert(mesh_session::stored_mac_must_drop(V::SHARED, true));
  /* A failed pubkey read registered nobody: every entry reads UNTRUSTED,
   * and none may go. */
  assert(!mesh_session::stored_mac_must_drop(V::BOUND, false));
  assert(!mesh_session::stored_mac_must_drop(V::REFUSED, false));
  assert(!mesh_session::stored_mac_must_drop(V::UNTRUSTED, false));
  assert(!mesh_session::stored_mac_must_drop(V::SHARED, false));
  std::printf("PASS test_stored_mac_must_drop_truth_table\n");
}

/* A raw peer_macs entry appended to the fake blob, bypassing upsert (which
 * since F102 refuses a held address): how a blob written before F102 looks. */
void raw_blob_entry(const uint8_t fp[8], const uint8_t mac[6]) {
  std::memcpy(g_nvs_macs + g_nvs_macs_len, fp, 8);
  std::memcpy(g_nvs_macs + g_nvs_macs_len + 8, mac, 6);
  g_nvs_macs_len += mesh_state::PEER_MAC_ENTRY_LEN;
}

bool heard_from(const uint8_t pub[32], const uint8_t priv[32], const uint8_t S[32],
                uint64_t counter, const uint8_t mac[6]) {
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(pub, priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, counter,
                                     frame, sizeof(frame));
  const size_t before = g_alerts_rx.size();
  mesh_transport::test::inject_recv(mac, frame, n, -40);
  mesh_transport::process();
  return g_alerts_rx.size() == before + 1;
}

/* F119. The blob F102's scenario left on firmware before F102: J paired at
 * X, C at its own address, then J re-paired from C's address, and the old
 * upsert rewrote J's entry (the first) in place — two entries, one address.
 * The restore binds neither (SHARED, SHARED) and drops both: neither member
 * is heard from that address, nobody holds it in the transport table, and
 * the blob is empty. Each member then re-pairs and is stored; after another
 * reboot C is heard at its address and J at X. In blob order (the code
 * before F119) J took C's address and C was not heard at all. Why drop
 * both rather than keep them unbound: with J's entry kept, C's re-pair
 * could not be stored (upsert refuses an address another fingerprint
 * holds, F102), and C would be unheard again after the next reboot. */
void test_boot_restore_binds_neither_member_of_a_shared_address() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x19 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x19, 0x00, 0x01};
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x19, 0x00, 0x0A};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x19, 0x00, 0x0C};
  uint8_t j_pub[32], j_priv[32], j_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  g_nvs_pubs.emplace_back(j_pub, j_pub + 32);
  g_nvs_pubs.emplace_back(c_pub, c_pub + 32);
  raw_blob_entry(j_fp, mac_c);
  raw_blob_entry(c_fp, mac_c);

  /* Why both go: with J's entry there, C's address cannot be stored. */
  {
    uint8_t blob[mesh_state::PEER_MACS_BLOB_MAX];
    size_t len = 0;
    assert(mesh_state::peer_mac_blob::upsert(blob, &len, j_fp, mac_c));
    assert(!mesh_state::peer_mac_blob::upsert(blob, &len, c_fp, mac_c));
  }

  size_t bound = 99;
  std::vector<mesh_session::StoredMacVerdict> v = main_like_boot(pub, priv, S, true, &bound);
  assert(v.size() == 2 && bound == 0);
  assert(v[0] == mesh_session::StoredMacVerdict::SHARED);
  assert(v[1] == mesh_session::StoredMacVerdict::SHARED);
  assert(mesh_session::trusted_peer_count() == 2);
  assert(!transport_has(mac_c));
  assert(!heard_from(c_pub, c_priv, S, 1, mac_c));
  assert(!heard_from(j_pub, j_priv, S, 2, mac_c));
  assert(g_nvs_macs_len == 0);                       /* both dropped */

  /* Each re-pairs from its own address, and is stored. */
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  assert(pair_from(S, me, mac_c, c_pub, c_priv, 100));
  assert(pair_from(S, me, mac_x, j_pub, j_priv, 200));
  assert(g_bound_calls.size() == 2 && g_bound_calls[0].bound && g_bound_calls[1].bound);
  assert(g_nvs_macs_len == 2 * mesh_state::PEER_MAC_ENTRY_LEN);

  v = main_like_boot(pub, priv, S, true, &bound);
  assert(bound == 2);
  assert(v[0] == mesh_session::StoredMacVerdict::BOUND && v[1] == mesh_session::StoredMacVerdict::BOUND);
  assert(heard_from(c_pub, c_priv, S, 3, mac_c));
  assert(heard_from(j_pub, j_priv, S, 4, mac_x));
  assert(!heard_from(j_pub, j_priv, S, 5, mac_c));
  std::printf("PASS test_boot_restore_binds_neither_member_of_a_shared_address\n");
}

/* F120. A member removed while its peer_macs entry stayed (the NVS removal
 * is best effort): its pubkey is gone from NVS, its entry is not. The
 * restore binds nothing for it (UNTRUSTED) and the entry is dropped; an
 * untrusted entry at a member's address does not make that member's entry
 * SHARED. A new member N then pairs from the removed one's address and is
 * stored, and after a reboot is heard there. Kept, the stale entry made
 * N's address impossible to store (upsert refuses it), so N was unheard
 * after every reboot. A boot whose pubkey list could not be read drops
 * nothing (and binds nothing: nobody is registered). */
void test_boot_restore_drops_entries_of_peers_no_longer_trusted() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x20 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x20, 0x00, 0x01};
  const uint8_t mac_1[6] = {0x24, 0x0A, 0xC4, 0x20, 0x00, 0x0A};
  const uint8_t mac_2[6] = {0x24, 0x0A, 0xC4, 0x20, 0x00, 0x0B};
  uint8_t x_pub[32], x_priv[32], x_fp[8], y_fp[8], c_pub[32], c_priv[32], c_fp[8];
  uint8_t n_pub[32], n_priv[32], n_fp[8], y_pub[32], y_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  assert(mesh_crypto::ed25519_generate_keypair(n_pub, n_priv));
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  mesh_crypto::compute_fingerprint(y_pub, y_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  mesh_crypto::compute_fingerprint(n_pub, n_fp);
  g_nvs_pubs.emplace_back(c_pub, c_pub + 32);       /* X and Y were removed */
  raw_blob_entry(x_fp, mac_1);
  raw_blob_entry(c_fp, mac_2);
  raw_blob_entry(y_fp, mac_2);

  /* A boot that could not read the pubkey list drops nothing. */
  const size_t len_before = g_nvs_macs_len;
  size_t bound = 99;
  std::vector<mesh_session::StoredMacVerdict> v = main_like_boot(pub, priv, S, false, &bound);
  assert(bound == 0 && g_nvs_macs_len == len_before);

  /* Why the entry must go: with X's entry there, N's address at mac_1
   * cannot be stored. */
  {
    uint8_t blob[mesh_state::PEER_MACS_BLOB_MAX];
    std::memcpy(blob, g_nvs_macs, g_nvs_macs_len);
    size_t len = g_nvs_macs_len;
    assert(!mesh_state::peer_mac_blob::upsert(blob, &len, n_fp, mac_1));
  }

  v = main_like_boot(pub, priv, S, true, &bound);
  assert(v.size() == 3 && bound == 1);
  assert(v[0] == mesh_session::StoredMacVerdict::UNTRUSTED);
  assert(v[1] == mesh_session::StoredMacVerdict::BOUND);
  assert(v[2] == mesh_session::StoredMacVerdict::UNTRUSTED);
  assert(!transport_has(mac_1));
  assert(heard_from(c_pub, c_priv, S, 1, mac_2));
  assert(!heard_from(x_pub, x_priv, S, 1, mac_1));
  assert(g_nvs_macs_len == mesh_state::PEER_MAC_ENTRY_LEN);   /* C's only */

  /* N pairs from X's old address: bound, stored, heard after a reboot. */
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  assert(pair_from(S, me, mac_1, n_pub, n_priv, 100));
  assert(g_bound_calls.size() == 1 && g_bound_calls[0].bound);
  assert(g_nvs_macs_len == 2 * mesh_state::PEER_MAC_ENTRY_LEN);
  v = main_like_boot(pub, priv, S, true, &bound);
  assert(bound == 2);
  assert(heard_from(n_pub, n_priv, S, 1, mac_1));
  assert(heard_from(c_pub, c_priv, S, 2, mac_2));
  std::printf("PASS test_boot_restore_drops_entries_of_peers_no_longer_trusted\n");
}

/* ── F70 — a member's frame is taken only from the member's own binding ──
 *
 * The transport table holds more addresses than a member's own: every
 * other member's, and, while a pairing runs, the partner's
 * (ensure_pair_contact) — which an outsider gets there by answering the
 * pairing from its own radio, no spoofing needed. Until F70 a member's
 * frame from ANY of them was verified, dispatched, and its source recorded
 * as the member's address (TrustedPeer::mac); send_rekey_frame unicast the
 * member's rekey replies there and forget_peer took it out of the table.
 * Since the envelope signs no address and one counter serves every
 * destination, a frame of the member's that this device had not heard yet
 * passed from there. So an outsider answering a pairing got this device's
 * REKEY_ACCEPT, and a radio copying another member's address made a later
 * removal of the signer strand that member. Now such a frame drops before
 * the signature check: nothing is recorded, nothing dispatched, no counter
 * spent. Each test below fails on the code before F70. */

/* Opera frames of `type` this device sent, to anyone. */
size_t sent_of_type(mesh_envelope::MsgType type) {
  size_t n = 0;
  for (const auto& o : g_outs) {
    if (o.bytes.size() >= mesh_envelope::MIN_FRAME_LEN &&
        o.bytes[mesh_envelope::OFFSET_VERSION] == mesh_envelope::OPERA_VERSION &&
        o.bytes[mesh_envelope::OFFSET_MSG_TYPE] == static_cast<uint8_t>(type)) {
      ++n;
    }
  }
  return n;
}

/* The frames this device sent to `to` (any kind). */
size_t sent_to(const uint8_t to[6]) {
  size_t n = 0;
  for (const auto& o : g_outs) n += std::memcmp(o.mac, to, 6) == 0;
  return n;
}

void test_pair_contact_replay_records_nothing_and_gets_no_accept() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xD7 + i);
  uint8_t a_pub[32], a_priv[32], a_fp[8];
  stand_up_session(S, a_pub, a_priv);           /* this device: A */
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  mesh_session::set_beacon_event_handler(on_beacon_event_received);
  mesh_session::set_peer_revoked_handler(on_peer_revoked);
  g_received.clear();
  g_revoked.clear();

  /* Members B and C, bound where they paired. */
  uint8_t b_pub[32], b_priv[32], b_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x00, 0x70, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x00, 0x70, 0x0C};
  assert(mesh_session::register_trusted_peer(b_pub) && mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(mesh_session::register_trusted_peer(c_pub) && mesh_session::bind_peer_mac(c_fp, mac_c));

  /* B's genuine frames that A never heard: a broadcast (counter 1), and the
   * REKEY_OFFER of B's removal of C, with A a survivor (counter 2). */
  uint8_t beacon[mesh_envelope::MAX_FRAME_LEN];
  const size_t beacon_len = build_beacon_frame(b_pub, b_priv, S, 1,
                                               mesh_beacon::BeaconState::ARRIVED, "porch",
                                               beacon, sizeof(beacon));
  mesh_rekey::Context cb;
  mesh_rekey::context_init(cb);
  uint8_t surv[1][mesh_crypto::FINGERPRINT_LEN];
  std::memcpy(surv[0], a_fp, sizeof(a_fp));
  mesh_rekey::Action b_offer = mesh_rekey::start(cb, b_fp, c_fp, surv, 1, 0x7070, 0);
  assert(b_offer.type == mesh_rekey::ActionType::BROADCAST_OFFER);
  uint8_t offer[mesh_envelope::MAX_FRAME_LEN];
  const size_t offer_len = build_signed_session_frame(b_pub, b_priv, S, 2,
                                                      mesh_envelope::MsgType::REKEY_OFFER,
                                                      b_offer.payload, b_offer.payload_len,
                                                      offer, sizeof(offer));

  /* Outsider O, its own key and its own radio: never a member. */
  uint8_t o_pub[32], o_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(o_pub, o_priv));
  const uint8_t mac_o[6] = {0x24, 0x0A, 0xC4, 0x00, 0x70, 0x0E};

  /* (1) A is the initiator; O answers with a DISCOVER as a joiner. */
  mesh_session::process(10);
  assert(mesh_session::start_pairing_initiator(S, "Home", 10));
  mesh_pairing::PairingContext co;
  mesh_pairing::context_init(co);
  const std::vector<uint8_t> disc = wire(mesh_pairing::start_joiner(co, o_pub, o_priv, 10));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_o, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_o));                /* the pairing partner, for the replies */
  assert(sent_to(mac_o) == 1);                  /* A's OFFER */

  /* O sends B's unheard broadcast and B's unheard OFFER from its own radio. */
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_o, beacon, beacon_len, -40);
  mesh_transport::test::inject_recv(mac_o, offer, offer_len, -40);
  mesh_transport::process();
  uint8_t seen[6];
  assert(g_received.empty());                   /* not dispatched */
  assert(!verified_link_mac(b_fp, seen));       /* nothing recorded for B */
  assert(!mesh_session::rekey_in_progress());   /* no rotation joined... */
  assert(!mesh_session::is_revoked(c_fp));      /* ...C neither revoked */
  assert(g_revoked.empty() && mesh_session::trusted_peer_count() == 2);
  assert(g_outs.empty());                       /* and no REKEY_ACCEPT, to O or anyone */
  assert(transport_has(mac_b) && transport_has(mac_c));

  /* The drops spent none of B's counter: B's own copies land, and the
   * ACCEPT goes to B's binding alone. */
  mesh_transport::test::inject_recv(mac_b, beacon, beacon_len, -40);
  mesh_transport::test::inject_recv(mac_b, offer, offer_len, -40);
  mesh_transport::process();
  assert(g_received.size() == 1);
  assert(verified_link_mac(b_fp, seen) && std::memcmp(seen, mac_b, 6) == 0);
  assert(mesh_session::rekey_in_progress() && mesh_session::is_revoked(c_fp));
  assert(sent_of_type(mesh_envelope::MsgType::REKEY_ACCEPT) == 1);
  assert(sent_to(mac_b) == 1 && sent_to(mac_o) == 0);
  mesh_session::cancel_pairing();
  assert(!transport_has(mac_o));
  mesh_rekey::wipe(b_offer);

  /* (2) A is the joiner (it already holds an opera); O sends it an OFFER as
   * an initiator, and A adds O's address for its ACCEPT. */
  stand_up_session(S, a_pub, a_priv);
  mesh_session::set_beacon_event_handler(on_beacon_event_received);
  g_received.clear();
  assert(mesh_session::register_trusted_peer(b_pub) && mesh_session::bind_peer_mac(b_fp, mac_b));
  uint8_t o_secret[32];
  for (size_t i = 0; i < sizeof(o_secret); ++i) o_secret[i] = (uint8_t)(0x0E + i);
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  mesh_pairing::Action a = mesh_pairing::start_initiator(ci, o_pub, o_priv, o_secret, "Evil", 10);
  assert(mesh_session::start_pairing_joiner(20));
  const std::vector<uint8_t> a_disc =
      last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  const uint8_t me[6] = {0x24, 0x0A, 0xC4, 0x00, 0x70, 0x0A};
  feed_pure(ci, me, a_disc, 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_OFFER);
  const std::vector<uint8_t> o_offer = wire(a);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_o, o_offer.data(), o_offer.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_o) && sent_to(mac_o) == 1);   /* A's ACCEPT */
  mesh_transport::test::inject_recv(mac_o, beacon, beacon_len, -40);
  mesh_transport::process();
  assert(g_received.empty() && !verified_link_mac(b_fp, seen));
  mesh_transport::test::inject_recv(mac_b, beacon, beacon_len, -40);
  mesh_transport::process();
  assert(g_received.size() == 1);
  mesh_session::cancel_pairing();
  std::printf("PASS test_pair_contact_replay_records_nothing_and_gets_no_accept\n");
}

/* A radio copying member C's bound address sends member B's unheard frame:
 * until F70 it was dispatched and B's link moved to C's address. */
void test_copied_member_address_moves_no_link() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xE3 + i);
  uint8_t a_pub[32], a_priv[32];
  stand_up_session(S, a_pub, a_priv);
  mesh_session::set_beacon_event_handler(on_beacon_event_received);
  g_received.clear();
  uint8_t b_pub[32], b_priv[32], b_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x00, 0x71, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x00, 0x71, 0x0C};
  assert(mesh_session::register_trusted_peer(b_pub) && mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(mesh_session::register_trusted_peer(c_pub) && mesh_session::bind_peer_mac(c_fp, mac_c));

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_beacon_frame(b_pub, b_priv, S, 1, mesh_beacon::BeaconState::ARRIVED, "hall",
                                frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_b, frame, n, -40);
  mesh_transport::process();
  uint8_t seen[6];
  assert(g_received.size() == 1);
  assert(verified_link_mac(b_fp, seen) && std::memcmp(seen, mac_b, 6) == 0);

  /* B's next frame, which A has not heard, from C's address. */
  n = build_beacon_frame(b_pub, b_priv, S, 2, mesh_beacon::BeaconState::DEPARTED, "hall",
                         frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);
  mesh_transport::process();
  assert(g_received.size() == 1);                                     /* not dispatched */
  assert(verified_link_mac(b_fp, seen) && std::memcmp(seen, mac_b, 6) == 0);   /* unmoved */
  assert(!verified_link_mac(c_fp, seen));                             /* nor credited to C */
  assert(transport_has(mac_b) && transport_has(mac_c));

  /* No counter spent: the same frame from B's binding lands. */
  mesh_transport::test::inject_recv(mac_b, frame, n, -40);
  mesh_transport::process();
  assert(g_received.size() == 2);
  assert(g_received[1].state == mesh_beacon::BeaconState::DEPARTED);
  std::printf("PASS test_copied_member_address_moves_no_link\n");
}

/* Forgetting a peer — remove_peer(), or a verified OFFER that names it —
 * takes its own radio MAC out of the transport table and no other address.
 * Until F70 forget_peer() also removed the address the peer's last verified
 * frame came from, which could be another member's. */
void test_forgetting_a_peer_drops_only_its_own_address() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xF1 + i);
  uint8_t a_pub[32], a_priv[32], a_fp[8];
  stand_up_session(S, a_pub, a_priv);
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  uint8_t b_pub[32], b_priv[32], b_fp[8], c_pub[32], c_priv[32], c_fp[8];
  uint8_t d_pub[32], d_priv[32], d_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  assert(mesh_crypto::ed25519_generate_keypair(d_pub, d_priv));
  mesh_crypto::compute_fingerprint(b_pub, b_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  mesh_crypto::compute_fingerprint(d_pub, d_fp);

  /* (a) The copy case, through remove_peer(). */
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x0C};
  const uint8_t mac_d[6] = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x0D};
  assert(mesh_session::register_trusted_peer(b_pub) && mesh_session::bind_peer_mac(b_fp, mac_b));
  assert(mesh_session::register_trusted_peer(c_pub) && mesh_session::bind_peer_mac(c_fp, mac_c));
  assert(mesh_session::register_trusted_peer(d_pub) && mesh_session::bind_peer_mac(d_fp, mac_d));
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(b_pub, b_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                               frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_b, frame, n, -40);
  n = build_alert_frame(b_pub, b_priv, S, 2, mesh_alert::Kind::TEMP_DRIFT, 3, 2,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);   /* B's frame, C's address */
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1);

  g_outs.clear();
  uint8_t removed[32];
  assert(mesh_session::remove_peer(b_fp, 100, removed) == mesh_session::RemoveResult::STARTED);
  assert(!transport_has(mac_b));
  assert(transport_has(mac_c) && transport_has(mac_d));      /* C is not stranded */
  assert(sent_of_type(mesh_envelope::MsgType::REKEY_OFFER) == 2);
  assert(sent_to(mac_c) == 1 && sent_to(mac_d) == 1);        /* the OFFER reaches both */
  n = build_alert_frame(c_pub, c_priv, S, 1, mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 3,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_c, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 2);                           /* and C is still heard */

  /* (b) A stale link, through a verified OFFER naming the peer. B is heard
   * at mac_1, then re-paired from mac_2; C, re-paired too, now sends from
   * mac_1 (two radio modules swapped). D's OFFER removes B: B's address,
   * mac_2, leaves the table, and C's, mac_1, stays. Until F70 B's link still
   * said mac_1 after the re-bind, and forgetting B removed it. */
  stand_up_session(S, a_pub, a_priv);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  const uint8_t mac_1[6]  = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x01};
  const uint8_t mac_2[6]  = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x02};
  const uint8_t mac_c0[6] = {0x24, 0x0A, 0xC4, 0x00, 0x72, 0x03};
  assert(mesh_session::register_trusted_peer(b_pub) && mesh_session::bind_peer_mac(b_fp, mac_1));
  assert(mesh_session::register_trusted_peer(c_pub) && mesh_session::bind_peer_mac(c_fp, mac_c0));
  assert(mesh_session::register_trusted_peer(d_pub) && mesh_session::bind_peer_mac(d_fp, mac_d));
  n = build_alert_frame(b_pub, b_priv, S, 3, mesh_alert::Kind::TEMP_DRIFT, 3, 4,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_1, frame, n, -40);
  mesh_transport::process();
  uint8_t seen[6];
  assert(verified_link_mac(b_fp, seen) && std::memcmp(seen, mac_1, 6) == 0);
  assert(mesh_session::bind_peer_mac(b_fp, mac_2));          /* B's re-pair */
  assert(!verified_link_mac(b_fp, seen));                    /* not heard at mac_2 yet */
  assert(mesh_session::bind_peer_mac(c_fp, mac_1));          /* C's re-pair */
  assert(transport_has(mac_1) && transport_has(mac_2) && !transport_has(mac_c0));

  mesh_rekey::Context cd;
  mesh_rekey::context_init(cd);
  uint8_t surv[1][mesh_crypto::FINGERPRINT_LEN];
  std::memcpy(surv[0], a_fp, sizeof(a_fp));
  mesh_rekey::Action d_offer = mesh_rekey::start(cd, d_fp, b_fp, surv, 1, 0x7272, 0);
  n = build_signed_session_frame(d_pub, d_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                 d_offer.payload, d_offer.payload_len, frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_d, frame, n, -40);
  mesh_transport::process();
  assert(mesh_session::is_revoked(b_fp));
  assert(mesh_session::trusted_peer_count() == 2);
  assert(!transport_has(mac_2));
  assert(transport_has(mac_1));                              /* C's address stays */
  n = build_alert_frame(c_pub, c_priv, S, 2, mesh_alert::Kind::CAMERA_TAMPER, 6, 5,
                        frame, sizeof(frame));
  const size_t before = g_alerts_rx.size();
  mesh_transport::test::inject_recv(mac_1, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == before + 1);                  /* and C is heard there */
  mesh_rekey::wipe(d_offer);
  std::printf("PASS test_forgetting_a_peer_drops_only_its_own_address\n");
}

/* A member with NO binding has no address its frames are taken from. That
 * state is real: main.cpp's boot restore registers every persisted pubkey
 * and binds only those with a peer_macs entry (none for NVS written before
 * F33 part 1, or after a save_peer_mac that failed), and a pairing whose
 * bind_peer_mac was refused (the address is another member's, or the
 * transport table is full) leaves the member registered and unbound. Until
 * F70 any address in the transport table delivered its frames: another
 * member's, or a running pairing's partner's. The source check's first
 * half (`!radio_mac_set`) is what this pins; the bound tests above never
 * reach it. */
void test_unbound_member_frame_is_never_taken() {
  uint8_t S[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x5B + i);
  uint8_t a_pub[32], a_priv[32], a_fp[8];
  stand_up_session(S, a_pub, a_priv);           /* this device: A */
  mesh_crypto::compute_fingerprint(a_pub, a_fp);
  mesh_session::set_beacon_event_handler(on_beacon_event_received);
  mesh_session::set_peer_revoked_handler(on_peer_revoked);
  g_received.clear();
  g_revoked.clear();

  /* U registered with no binding; C bound where it paired. */
  uint8_t u_pub[32], u_priv[32], u_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(u_pub, u_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(u_pub, u_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  const uint8_t mac_u[6] = {0x24, 0x0A, 0xC4, 0x00, 0x73, 0x0A};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x00, 0x73, 0x0C};
  const uint8_t mac_o[6] = {0x24, 0x0A, 0xC4, 0x00, 0x73, 0x0E};
  assert(mesh_session::register_trusted_peer(u_pub));
  assert(mesh_session::register_trusted_peer(c_pub) && mesh_session::bind_peer_mac(c_fp, mac_c));

  /* U's frames that A never heard: a broadcast (counter 1), and the
   * REKEY_OFFER of U's removal of C, with A a survivor (counter 2). */
  uint8_t beacon[mesh_envelope::MAX_FRAME_LEN];
  const size_t beacon_len = build_beacon_frame(u_pub, u_priv, S, 1,
                                               mesh_beacon::BeaconState::ARRIVED, "shed",
                                               beacon, sizeof(beacon));
  mesh_rekey::Context cu;
  mesh_rekey::context_init(cu);
  uint8_t surv[1][mesh_crypto::FINGERPRINT_LEN];
  std::memcpy(surv[0], a_fp, sizeof(a_fp));
  mesh_rekey::Action u_offer = mesh_rekey::start(cu, u_fp, c_fp, surv, 1, 0x7373, 0);
  assert(u_offer.type == mesh_rekey::ActionType::BROADCAST_OFFER);
  uint8_t offer[mesh_envelope::MAX_FRAME_LEN];
  const size_t offer_len = build_signed_session_frame(u_pub, u_priv, S, 2,
                                                      mesh_envelope::MsgType::REKEY_OFFER,
                                                      u_offer.payload, u_offer.payload_len,
                                                      offer, sizeof(offer));

  /* (a) U's broadcast from member C's address (a radio copying it). */
  mesh_transport::test::inject_recv(mac_c, beacon, beacon_len, -40);
  mesh_transport::process();
  uint8_t seen[6];
  assert(g_received.empty());                   /* not dispatched */
  assert(!verified_link_mac(u_fp, seen));       /* nothing recorded for U */
  assert(!verified_link_mac(c_fp, seen));       /* nor credited to C */
  assert(transport_has(mac_c));

  /* (b) Outsider O answers A's pairing from its own radio, then sends U's
   * OFFER. Until F70: A joined the rotation, revoked C, took C's address
   * out of the table, and sent its REKEY_ACCEPT to O, the only address
   * left. */
  uint8_t o_pub[32], o_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(o_pub, o_priv));
  mesh_session::process(10);
  assert(mesh_session::start_pairing_initiator(S, "Home", 10));
  mesh_pairing::PairingContext co;
  mesh_pairing::context_init(co);
  const std::vector<uint8_t> disc = wire(mesh_pairing::start_joiner(co, o_pub, o_priv, 10));
  mesh_transport::test::inject_recv(mac_o, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_o));                 /* the pairing partner */
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_o, offer, offer_len, -40);
  mesh_transport::process();
  assert(!mesh_session::rekey_in_progress());   /* no rotation joined... */
  assert(!mesh_session::is_revoked(c_fp));      /* ...C neither revoked */
  assert(g_revoked.empty() && mesh_session::trusted_peer_count() == 2);
  assert(transport_has(mac_c));                 /* nor taken out of the table */
  assert(g_outs.empty());                       /* and no REKEY_ACCEPT, to O or anyone */
  assert(!verified_link_mac(u_fp, seen));
  mesh_session::cancel_pairing();
  assert(!transport_has(mac_o));

  /* Neither drop spent U's counter: bound, U's own copies land, and the
   * ACCEPT goes to U's binding alone. */
  assert(mesh_session::bind_peer_mac(u_fp, mac_u));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_u, beacon, beacon_len, -40);
  mesh_transport::test::inject_recv(mac_u, offer, offer_len, -40);
  mesh_transport::process();
  assert(g_received.size() == 1);
  assert(verified_link_mac(u_fp, seen) && std::memcmp(seen, mac_u, 6) == 0);
  assert(mesh_session::rekey_in_progress() && mesh_session::is_revoked(c_fp));
  assert(sent_of_type(mesh_envelope::MsgType::REKEY_ACCEPT) == 1);
  assert(g_outs.size() == 1 && sent_to(mac_u) == 1);
  mesh_rekey::wipe(u_offer);
  std::printf("PASS test_unbound_member_frame_is_never_taken\n");
}

/* ── F101 — the opera senders reach bound members only ──────────────────
 *
 * Until F101 every opera sender used mesh_transport::broadcast(), which
 * sends to every address in the transport table. While a pairing runs the
 * table also holds the partner's address (ensure_pair_contact), and an
 * outsider gets its address there by answering the pairing from its own
 * radio. So the opera's tamper alerts, beacon events, channel locks, hub
 * elections, rekey OFFERs and a LEAVE went to it too, and counted as sent:
 * with no member at all, send_tamper_alert() returned true and
 * leave_opera() reported notified. Each test below fails on the code
 * before F101. */

/* Start a pairing as initiator and have an outsider at `mac_o` answer it:
 * its address is now in the transport table as the pairing partner. */
void outsider_answers_the_pairing(const uint8_t S[32], const uint8_t mac_o[6]) {
  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  uint8_t o_pub[32], o_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(o_pub, o_priv));
  mesh_pairing::PairingContext co;
  mesh_pairing::context_init(co);
  const std::vector<uint8_t> disc = wire(mesh_pairing::start_joiner(co, o_pub, o_priv, 20));
  mesh_transport::test::inject_recv(mac_o, disc.data(), disc.size(), -40);
  mesh_transport::process();
  assert(transport_has(mac_o));
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_ACCEPT);
}

/* With no member, nothing is sent and nothing reports sent, though the
 * pairing partner's address is in the table. */
void test_opera_sends_with_only_a_pairing_partner_reach_nobody() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x11 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  const uint8_t mac_o[6] = {0x24, 0x0A, 0xC4, 0x01, 0x01, 0x0E};
  outsider_answers_the_pairing(S, mac_o);

  /* A trusted member with no binding is not sent to either. */
  uint8_t u_pub[32], u_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(u_pub, u_priv));
  assert(mesh_session::register_trusted_peer(u_pub));

  g_outs.clear();
  uint8_t fp[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 1, 30));
  assert(!mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "door", 31));
  assert(!mesh_session::send_channel_lock(6, mesh_channel_hop::Reason::UTILIZATION, 32));
  assert(!mesh_session::send_hub_election(mesh_hub_election::Event::HUB_ELECTED, fp, 33));
  assert(g_outs.empty());
  assert(!mesh_session::leave_opera(40));
  assert(sent_of_type(mesh_envelope::MsgType::LEAVE_OPERA) == 0);
  assert(sent_to(mac_o) == 0);
  std::printf("PASS test_opera_sends_with_only_a_pairing_partner_reach_nobody\n");
}

/* With a bound member B, an unbound member U and the outsider answering a
 * pairing, each opera sender sends exactly one frame, to B, and the LEAVE
 * too. */
void test_opera_sends_reach_bound_members_only() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x21 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x01, 0x02, 0x0B};
  const uint8_t mac_o[6] = {0x24, 0x0A, 0xC4, 0x01, 0x02, 0x0E};
  add_bound_member(mac_b);
  uint8_t u_pub[32], u_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(u_pub, u_priv));
  assert(mesh_session::register_trusted_peer(u_pub));
  outsider_answers_the_pairing(S, mac_o);

  uint8_t fp[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  g_outs.clear();
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 1, 30));
  assert(g_outs.size() == 1 && sent_to(mac_b) == 1);
  g_outs.clear();
  assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "door", 31));
  assert(g_outs.size() == 1 && sent_to(mac_b) == 1);
  g_outs.clear();
  assert(mesh_session::send_channel_lock(6, mesh_channel_hop::Reason::UTILIZATION, 32));
  assert(g_outs.size() == 1 && sent_to(mac_b) == 1);
  g_outs.clear();
  assert(mesh_session::send_hub_election(mesh_hub_election::Event::HUB_ELECTED, fp, 33));
  assert(g_outs.size() == 1 && sent_to(mac_b) == 1);

  /* A rotation's OFFER, re-sent while the pairing runs, goes to B alone.
   * (POST /api/mesh/pair/start refuses to start a pairing during a
   * rotation; the session API itself does not, so the overlap is set up
   * here directly.) */
  mesh_session::cancel_pairing();
  uint8_t c_pub[32], c_priv[32], c_fp[8], removed[32];
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  assert(mesh_session::register_trusted_peer(c_pub));
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  assert(mesh_session::remove_peer(c_fp, 100, removed) == mesh_session::RemoveResult::STARTED);
  outsider_answers_the_pairing(S, mac_o);
  g_outs.clear();
  mesh_session::process(100 + mesh_rekey::REKEY_RETRY_MS);
  assert(sent_of_type(mesh_envelope::MsgType::REKEY_OFFER) == 1);
  assert(g_outs.size() == 1 && sent_to(mac_b) == 1);

  g_outs.clear();
  assert(mesh_session::leave_opera(200));
  assert(sent_of_type(mesh_envelope::MsgType::LEAVE_OPERA) == 1);
  assert(sent_to(mac_b) == 1 && sent_to(mac_o) == 0);
  std::printf("PASS test_opera_sends_reach_bound_members_only\n");
}

/* A send hook that records every attempt (in g_outs) and refuses those to
 * the addresses in g_refuse_to, as a driver whose send failed would. */
std::vector<std::vector<uint8_t>> g_refuse_to;
bool refusing_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  capture_send(mac, data, len);
  for (const auto& r : g_refuse_to) {
    if (std::memcmp(r.data(), mac, 6) == 0) return false;
  }
  return true;
}

/* Only the sends the transport took count as sent. Two bound members, B and
 * C: with B's sends refused, every sender still reports sent (C's copy was
 * taken); with both refused, none does, though one copy per member was
 * tried, and leave_opera() reports not notified. Fails when send_to_members
 * counts attempts instead of successes: the senders then report sent with
 * nothing sent, the false "sent" F101 removed. */
void test_opera_sends_count_only_what_the_transport_took() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x31 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x01, 0x03, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x01, 0x03, 0x0C};
  add_bound_member(mac_b);
  add_bound_member(mac_c);
  mesh_transport::test::set_send_hook(refusing_send);
  uint8_t fp[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  for (int refuse_both = 0; refuse_both < 2; ++refuse_both) {
    g_refuse_to.clear();
    g_refuse_to.push_back(std::vector<uint8_t>(mac_b, mac_b + 6));
    if (refuse_both) g_refuse_to.push_back(std::vector<uint8_t>(mac_c, mac_c + 6));
    const bool expect = !refuse_both;
    const uint32_t t = 30 + 10 * (uint32_t)refuse_both;
    g_outs.clear();
    assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 1, t) == expect);
    assert(mesh_session::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "door", t + 1) == expect);
    assert(mesh_session::send_channel_lock(6, mesh_channel_hop::Reason::UTILIZATION, t + 2) == expect);
    assert(mesh_session::send_hub_election(mesh_hub_election::Event::HUB_ELECTED, fp, t + 3) == expect);
    assert(g_outs.size() == 8 && sent_to(mac_b) == 4 && sent_to(mac_c) == 4);   /* all tried */
  }
  g_outs.clear();
  assert(!mesh_session::leave_opera(100));
  assert(sent_to(mac_b) == 1 && sent_to(mac_c) == 1);
  g_refuse_to.clear();
  mesh_transport::test::set_send_hook(capture_send);
  std::printf("PASS test_opera_sends_count_only_what_the_transport_took\n");
}

/* ── F118 — a pairing whose partner this device cannot hold fails ─────────
 *
 * Spec §5.2: a device that cannot hold its partner fails the pairing. Until
 * F118 the PIO session asked nothing before the end: the initiator sealed
 * the opera_secret into its COMPLETE, both sides reported PAIRED,
 * main.cpp's PairedCallback registered and persisted the partner's key, and
 * only then did the session try the bind — refused for an address another
 * member holds, and the callback's register refused for a new member when
 * the trusted table was full. The partner became a member heard from
 * nowhere (F70), sent nothing (F101), holding a slot until removed (F102
 * kept its address out of NVS). Now the session hands every pairing
 * can_hold_partner as its PartnerGate: asked at the owner's confirm before
 * any CONFIRM goes out, on the initiator again before it seals, on the
 * joiner again before it opens a COMPLETE. */

/* Drive this session as the INITIATOR, with a pure joiner (j_pub, j_priv)
 * at mac_j, up to both codes shown. */
void initiator_to_code(const uint8_t S[32], const uint8_t me[6], const uint8_t mac_j[6],
                       const uint8_t j_pub[32], const uint8_t j_priv[32], uint32_t t,
                       mesh_pairing::PairingContext& cj) {
  mesh_pairing::context_init(cj);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, t);
  const std::vector<uint8_t> disc = wire(a);
  assert(mesh_session::start_pairing_initiator(S, "Home", t));
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  feed_pure(cj, me, last_to(mac_j), t + 1, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);
}

/* The PAIR_CONFIRM request, through the REST slot as the handler sends it. */
mesh_session::RequestStatus rest_confirm(uint32_t now) {
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CONFIRM)));
  mesh_session::process(now);
  mesh_session::RequestResult res;
  assert(mesh_session::take_request_result(&res));
  return res.status;
}

/* As the INITIATOR, for the two refusals a pairing can reach — a re-pair of
 * member J from member C's address, and a new member N while eight are
 * trusted — in both confirm orders: the owner's confirm through the REST
 * slot answers PARTNER_REFUSED; the FailedCallback gets PARTNER_REFUSED and
 * the partner's fingerprint; nothing goes to the partner (no CONFIRM, no
 * COMPLETE: no opera_secret is sealed); neither paired callback runs; the
 * trusted table, the bindings and the fake NVS are as they were (J heard at
 * X, C at its own address); a new partner's address leaves the transport
 * table; and the joiner, which never gets a COMPLETE, fails at its
 * timeout. On the code before F118 each case reported PAIRED, sent the
 * COMPLETE, and main.cpp's callback registered and stored the partner. */
void test_a_partner_the_initiator_cannot_hold_fails_the_pairing() {
  for (int full_opera = 0; full_opera < 2; ++full_opera) {
    for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
      uint8_t S[32];
      for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x18 + i);
      uint8_t pub[32], priv[32];
      stand_up_session(S, pub, priv);
      reset_fake_main_nvs();
      mesh_session::set_paired_callback(main_like_paired);
      mesh_session::set_paired_peer_bound_callback(main_like_bound);
      mesh_session::set_tamper_alert_handler(on_alert_rx);
      const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x18, 0x00, 0x01};
      const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x18, 0x00, 0x0A};
      const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x18, 0x00, 0x0C};
      const uint8_t mac_n[6] = {0x24, 0x0A, 0xC4, 0x18, 0x00, 0x0D};
      uint8_t j_pub[32], j_priv[32], c_pub[32], c_priv[32], n_pub[32], n_priv[32];
      assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
      assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
      assert(mesh_crypto::ed25519_generate_keypair(n_pub, n_priv));
      assert(pair_from(S, me, mac_x, j_pub, j_priv, 100));
      assert(pair_from(S, me, mac_c, c_pub, c_priv, 200));
      if (full_opera) {
        for (int i = 0; i < 5; ++i) {
          const uint8_t m[6] = {0x24, 0x0A, 0xC4, 0x18, 0x01, (uint8_t)i};
          add_bound_member(m);
        }
        uint8_t u_pub[32], u_priv[32];   /* the eighth, unbound: the transport has a slot */
        assert(mesh_crypto::ed25519_generate_keypair(u_pub, u_priv));
        assert(mesh_session::register_trusted_peer(u_pub));
      }
      const size_t trusted = mesh_session::trusted_peer_count();
      const uint8_t* p_pub   = full_opera ? n_pub : j_pub;
      const uint8_t* p_priv  = full_opera ? n_priv : j_priv;
      const uint8_t* p_mac   = full_opera ? mac_n : mac_c;
      uint8_t p_fp[8];
      mesh_crypto::compute_fingerprint(p_pub, p_fp);
      assert(!mesh_session::can_hold_partner(p_pub, p_mac));

      mesh_pairing::PairingContext cj;
      initiator_to_code(S, me, p_mac, p_pub, p_priv, 300, cj);
      g_failed_fired = false;
      g_paired_fired = false;
      g_outs.clear();
      std::vector<uint8_t> conf_j;
      if (joiner_first) {
        mesh_pairing::Action a = mesh_pairing::confirm_code(cj, 310);
        conf_j = wire(a);
        mesh_transport::test::inject_recv(p_mac, conf_j.data(), conf_j.size(), -40);
        mesh_transport::process();
        assert(g_outs.empty() && !g_failed_fired);
      }
      assert(rest_confirm(320) == mesh_session::RequestStatus::PARTNER_REFUSED);
      assert(g_failed_fired && g_failed_why == mesh_pairing::FailReason::PARTNER_REFUSED);
      assert(g_failed_fp.size() == 8 && std::memcmp(g_failed_fp.data(), p_fp, 8) == 0);
      assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
      assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::PARTNER_REFUSED);
      if (!joiner_first) {
        /* The joiner's owner confirms after: its CONFIRM is dropped. */
        mesh_pairing::Action a = mesh_pairing::confirm_code(cj, 330);
        conf_j = wire(a);
        mesh_transport::test::inject_recv(p_mac, conf_j.data(), conf_j.size(), -40);
        mesh_transport::process();
      }
      mesh_session::process(340);
      assert(sent_to(p_mac) == 0);                 /* no CONFIRM, no COMPLETE */
      assert(!g_paired_fired && g_bound_calls.size() == 2);
      assert(mesh_session::trusted_peer_count() == trusted);
      assert(g_nvs_pubs.size() == 2);
      mesh_state::PeerMac stored[mesh_state::MAX_TRUSTED_PEERS];
      size_t n_stored = 0;
      assert(mesh_state::peer_mac_blob::decode(g_nvs_macs, g_nvs_macs_len, stored,
                                               mesh_state::MAX_TRUSTED_PEERS, &n_stored));
      assert(n_stored == 2);
      if (full_opera) assert(!transport_has(mac_n));   /* the pair contact went */
      /* J is still heard at X and C at its own address. */
      g_alerts_rx.clear();
      uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
      size_t n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                                   frame, sizeof(frame));
      mesh_transport::test::inject_recv(mac_x, frame, n, -40);
      n = build_alert_frame(c_pub, c_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 2,
                            frame, sizeof(frame));
      mesh_transport::test::inject_recv(mac_c, frame, n, -40);
      mesh_transport::process();
      assert(g_alerts_rx.size() == 2);
      /* The joiner never got a COMPLETE: it fails at its timeout. */
      mesh_pairing::Action a = mesh_pairing::tick(cj, 300 + mesh_pairing::PAIRING_TIMEOUT_MS);
      assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
      assert(!cj.opera_secret_present);
    }
  }
  std::printf("PASS test_a_partner_the_initiator_cannot_hold_fails_the_pairing"
              "  (held address, full opera; both orders)\n");
}

/* As the INITIATOR, asked again before the seal: the owner confirms first
 * and is admitted (its CONFIRM goes out); the trusted table then fills
 * before the joiner's CONFIRM arrives, and that CONFIRM ends the pairing
 * with PARTNER_REFUSED and no COMPLETE. Fails with the gate removed from
 * mesh_pairing's initiator_complete. */
void test_the_initiator_asks_again_before_it_seals() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x28 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x18, 0x10, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x18, 0x10, 0x02};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  initiator_to_code(S, me, mac_j, j_pub, j_priv, 100, cj);
  g_outs.clear();
  assert(rest_confirm(110) == mesh_session::RequestStatus::OK);
  assert(sent_to(mac_j) == 1);                   /* its CONFIRM */
  for (size_t i = 0; i < mesh_session::MAX_TRUSTED_PEERS; ++i) {
    uint8_t k_pub[32], k_priv[32];
    assert(mesh_crypto::ed25519_generate_keypair(k_pub, k_priv));
    assert(mesh_session::register_trusted_peer(k_pub));
  }
  mesh_pairing::Action a = mesh_pairing::confirm_code(cj, 120);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  mesh_session::process(130);
  assert(sent_to(mac_j) == 1);                   /* no COMPLETE */
  assert(g_failed_fired && g_failed_why == mesh_pairing::FailReason::PARTNER_REFUSED);
  assert(!g_paired_fired);
  assert(!transport_has(mac_j));
  std::printf("PASS test_the_initiator_asks_again_before_it_seals\n");
}

/* As the JOINER. First, refused at its owner's confirm: a member of this
 * device (it joins another opera without leaving its own) is bound at the
 * initiator's address. The confirm answers PARTNER_REFUSED and sends no
 * CONFIRM, so the initiator never seals the secret to it: it waits and
 * fails at its timeout (canary-wap's joiner refuses only at the COMPLETE,
 * after its initiator has added it, F73). Then refused at the COMPLETE: the
 * owner's confirm is admitted, the trusted table fills before the COMPLETE
 * arrives, and the COMPLETE ends the pairing: no PairedCallback, no secret,
 * no opera. On the code before F118 both reported PAIRED and installed the
 * initiator's secret. */
void test_a_joiner_that_cannot_hold_its_initiator_fails_the_pairing() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x38 + i);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x18, 0x20, 0x01};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x18, 0x20, 0x02};
  for (int at_complete = 0; at_complete < 2; ++at_complete) {
    uint8_t pub[32], priv[32];
    stand_up_session(nullptr, pub, priv);
    mesh_session::set_paired_callback(on_paired_register);
    if (!at_complete) add_bound_member(mac_i);
    uint8_t i_pub[32], i_priv[32];
    assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
    mesh_pairing::PairingContext ci;
    mesh_pairing::context_init(ci);
    mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 10);
    assert(mesh_session::start_pairing_joiner(20));
    feed_pure(ci, me, last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), 30, &a);
    const std::vector<uint8_t> offer = wire(a);
    mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
    mesh_transport::process();
    feed_pure(ci, me, last_to(mac_i), 40, &a);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
    g_outs.clear();
    if (!at_complete) {
      assert(rest_confirm(50) == mesh_session::RequestStatus::PARTNER_REFUSED);
      assert(g_outs.empty());                    /* no CONFIRM */
      assert(g_failed_fired && g_failed_why == mesh_pairing::FailReason::PARTNER_REFUSED);
      a = mesh_pairing::confirm_code(ci, 60);
      assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
      const std::vector<uint8_t> conf_i = wire(a);
      mesh_transport::test::inject_recv(mac_i, conf_i.data(), conf_i.size(), -40);
      mesh_transport::process();
      assert(g_outs.empty());
      a = mesh_pairing::tick(ci, 10 + mesh_pairing::PAIRING_TIMEOUT_MS);
      assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);   /* never sealed */
      assert(transport_has(mac_i));              /* the member's binding stays */
    } else {
      assert(rest_confirm(50) == mesh_session::RequestStatus::OK);
      assert(g_outs.size() == 1);                /* its CONFIRM */
      for (size_t i = 0; i < mesh_session::MAX_TRUSTED_PEERS; ++i) {
        uint8_t k_pub[32], k_priv[32];
        assert(mesh_crypto::ed25519_generate_keypair(k_pub, k_priv));
        assert(mesh_session::register_trusted_peer(k_pub));
      }
      feed_pure(ci, me, g_outs[0].bytes, 60, &a);
      assert(a.type == mesh_pairing::ActionType::NONE);
      a = mesh_pairing::confirm_code(ci, 70);
      assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
      inject_all(mac_i, wire_all(a));
      assert(g_failed_fired && g_failed_why == mesh_pairing::FailReason::PARTNER_REFUSED);
      assert(!transport_has(mac_i));             /* the pair contact went */
    }
    assert(!g_paired_fired && !g_paired_with_secret);
    assert(!mesh_session::has_opera());
    assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  }
  std::printf("PASS test_a_joiner_that_cannot_hold_its_initiator_fails_the_pairing"
              "  (at the confirm, at the COMPLETE)\n");
}

/* ── F33 part 3 — the outbound counter survives a reboot ──────────────── */

/* A fake NVS for mesh_state::save/load_outbound_counter. */
uint32_t g_ctr_clock = 0;   /* the transport's virtual clock for these tests */
uint64_t g_nvs_ctr = 0;
bool     g_nvs_has = false;
int      g_nvs_writes = 0;
bool     g_nvs_fail = false;
bool fake_reserve(uint64_t high) {
  if (g_nvs_fail) return false;
  g_nvs_ctr = high;
  g_nvs_has = true;
  ++g_nvs_writes;
  return true;
}

uint64_t frame_counter(const std::vector<uint8_t>& f) {
  assert(f.size() > mesh_envelope::OFFSET_COUNTER + 8);
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | f[mesh_envelope::OFFSET_COUNTER + i];
  return v;
}

/* Boot the singleton as the same device (same keys), the way main.cpp
 * does: restore the persisted mark BEFORE anything sends, then install
 * the reserve handler. RAM state from the previous "life" is gone. */
const uint8_t kCtrPeer[6] = {0x24, 0x0A, 0xC4, 0x00, 0x05, 0x01};
void boot_device(const uint8_t pub[32], const uint8_t priv[32], const uint8_t S[32]) {
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv));
  assert(mesh_session::start());
  assert(mesh_session::set_opera_secret(S));
  if (g_nvs_has) mesh_session::restore_outbound_counter(g_nvs_ctr);
  mesh_session::set_counter_reserve_handler(fake_reserve);
  add_bound_member(kCtrPeer);   /* someone to send to */
  /* reset_world() rewound the transport clock; keep it moving forward so
   * the storm limiter's window logic sees real time. */
  g_ctr_clock += 60000;
  mesh_transport::test::set_now_ms(g_ctr_clock);
}

/* How many sends reach the durable mark from here. A counter already past
 * the mark is itself the bug (a counter used before it was reserved). */
size_t sends_to_mark() {
  assert(mesh_session::outbound_counter() <= g_nvs_ctr);
  return (size_t)(g_nvs_ctr - mesh_session::outbound_counter());
}

/* Sends `n` alerts; returns the counters that went on air. The virtual
 * clock steps 20 ms per send so the transport's storm limiter (100/s)
 * never trips. */
std::vector<uint64_t> send_alerts(size_t n) {
  std::vector<uint64_t> used;
  for (size_t i = 0; i < n; ++i) {
    g_ctr_clock += 20;
    mesh_transport::test::set_now_ms(g_ctr_clock);
    g_outs.clear();
    if (mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, (uint32_t)i, 10)) {
      assert(g_outs.size() == 1);
      used.push_back(frame_counter(g_outs[0].bytes));
    }
  }
  return used;
}

void test_outbound_counter_reserve_ahead() {
  uint8_t pub[32], priv[32], S[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xC1 + i);
  g_nvs_ctr = 0; g_nvs_has = false; g_nvs_writes = 0; g_nvs_fail = false;
  const uint64_t B = mesh_session::COUNTER_RESERVE_BLOCK;

  /* Life 1: 2500 frames cost ceil(2500 / B) NVS writes, not 2500, and the
   * persisted mark is always at or above every counter used. */
  boot_device(pub, priv, S);
  std::vector<uint64_t> used = send_alerts(2500);
  assert(used.size() == 2500 && used.front() == 1 && used.back() == 2500);
  assert(g_nvs_writes == (int)((2500 + B - 1) / B));
  assert(g_nvs_ctr >= 2500);
  uint64_t max_used = used.back();

  /* Crash between reserve and use, at every point of a block: send up to
   * the edge of the reservation, then the frame that crosses it (the new
   * reservation is written, the frame is "lost" — never went on air),
   * crash, reboot. The first counter after the reboot is above every
   * counter used AND above the reserved one: no reuse, whatever was lost. */
  for (int round = 0; round < 3; ++round) {
    const uint64_t edge = g_nvs_ctr;
    std::vector<uint64_t> more = send_alerts(sends_to_mark());
    if (!more.empty()) max_used = more.back();
    assert(mesh_session::outbound_counter() == edge);
    const int writes_before = g_nvs_writes;
    g_outs.clear();
    mesh_transport::test::set_now_ms(g_ctr_clock += 20);
    assert(mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 10));
    assert(g_nvs_writes == writes_before + 1);      /* reserved, then used */
    const uint64_t lost = frame_counter(g_outs[0].bytes);
    assert(lost == edge + 1 && g_nvs_ctr == edge + B);
    /* CRASH: RAM gone. */
    boot_device(pub, priv, S);
    std::vector<uint64_t> after = send_alerts(1);
    assert(after.size() == 1);
    assert(after[0] > max_used && after[0] > lost);
    max_used = after[0];
  }

  /* A crash right after a reboot, before anything was sent, and again:
   * each reboot resumes above the persisted mark, never below. */
  const uint64_t mark = g_nvs_ctr;
  boot_device(pub, priv, S);
  boot_device(pub, priv, S);
  std::vector<uint64_t> after = send_alerts(1);
  assert(after.size() == 1 && after[0] > mark && after[0] > max_used);
  max_used = after[0];

  /* A refused reservation refuses the frame (fail closed), and nothing is
   * used past the durable mark; the next send that can persist goes on. */
  const uint64_t edge = g_nvs_ctr;
  send_alerts(sends_to_mark());
  g_nvs_fail = true;
  g_outs.clear();
  mesh_transport::test::set_now_ms(g_ctr_clock += 20);
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 10));
  assert(g_outs.empty() && mesh_session::outbound_counter() == edge);
  /* And again, with NVS still failing: a refused reservation is not a held
   * one. Code that marked the block reserved before its persist succeeded
   * would sign this frame above the durable mark, and the reboot below would
   * then reuse that counter. */
  mesh_transport::test::set_now_ms(g_ctr_clock += 20);
  assert(!mesh_session::send_tamper_alert(mesh_alert::Kind::TEMP_DRIFT, 3, 0, 10));
  assert(g_outs.empty() && mesh_session::outbound_counter() == edge);
  boot_device(pub, priv, S);               /* crash while NVS was failing */
  g_nvs_fail = false;
  after = send_alerts(1);
  assert(after.size() == 1 && after[0] == edge + 1);   /* edge+1 was never used */

  /* End to end: a receiver that remembers this sender's last counter (its
   * replay_ctrs) accepts the rebooted sender's next frame. */
  const uint64_t last_seen = after[0];
  boot_device(pub, priv, S);
  g_outs.clear();
  mesh_transport::test::set_now_ms(g_ctr_clock += 20);
  assert(mesh_session::send_tamper_alert(mesh_alert::Kind::CAMERA_TAMPER, 6, 42, 10));
  const std::vector<uint8_t> post_reboot = g_outs[0].bytes;
  uint8_t rx_pub[32], rx_priv[32];
  stand_up_session(S, rx_pub, rx_priv);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  uint8_t tx_fp[8];
  mesh_crypto::compute_fingerprint(pub, tx_fp);
  assert(mesh_session::register_trusted_peer(pub));
  assert(mesh_session::restore_replay_counter(tx_fp, last_seen));
  const uint8_t tx_mac[6] = {0x24, 0x0A, 0xC4, 0x00, 0x05, 0x02};
  assert(mesh_session::bind_peer_mac(tx_fp, tx_mac));
  mesh_transport::test::inject_recv(tx_mac, post_reboot.data(), post_reboot.size(), -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1 && g_alerts_rx[0].witness_seq == 42);
  std::printf("PASS test_outbound_counter_reserve_ahead  (%d NVS writes)\n", g_nvs_writes);
}

/* Without the reservation the same reboot reuses counters — the bug. Kept
 * as a control so the test above is known to see the difference. */
void test_outbound_counter_without_reservation_restarts() {
  uint8_t pub[32], priv[32], S[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xD1 + i);
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv) && mesh_session::start() && mesh_session::set_opera_secret(S));
  add_bound_member(kCtrPeer);
  std::vector<uint64_t> used = send_alerts(5);
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv) && mesh_session::start() && mesh_session::set_opera_secret(S));
  add_bound_member(kCtrPeer);
  std::vector<uint64_t> after = send_alerts(1);
  assert(after[0] <= used.back());   /* a reused counter: dropped as a replay */
  std::printf("PASS test_outbound_counter_without_reservation_restarts\n");
}

/* ── F33 part 5 — the pairing routes run on the main loop ─────────────── */

bool g_abandon_in_failed = false;
void on_failed_abandoning(mesh_pairing::FailReason why, const uint8_t* fp) {
  on_failed(why, fp);
  /* The handler gives up while its request is still RUNNING. */
  if (g_abandon_in_failed) mesh_session::abandon_request();
}

/* F33 part 4 — spec §5.4: POST /api/mesh/pair/start on a device with no
 * opera founds one on the main loop (canary-wap's create-on-start). The
 * secret is drawn there, persisted through the handler BEFORE anything uses
 * it, and is the one a joiner then receives; nothing is created when it
 * cannot be persisted, and an opera the session holds is never replaced. */
int                  g_create_calls = 0;
bool                 g_create_ok    = true;
std::vector<uint8_t> g_created_secret;
std::string          g_created_name;
bool on_opera_create(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN], const char* name) {
  ++g_create_calls;
  g_created_secret.assign(secret, secret + mesh_crypto::OPERA_SECRET_LEN);
  g_created_name = name != nullptr ? name : "";
  return g_create_ok;
}

void test_rest_pair_start_creates_opera() {
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::RequestResult res;
  mesh_session::Request create = make_request(mesh_session::RequestType::PAIR_START);
  create.create = true;
  g_create_calls = 0;

  /* No handler: nothing can be persisted, so nothing is created. */
  g_outs.clear();
  assert(mesh_session::submit_request(create));
  mesh_session::process(10);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::NOT_PERSISTED && !res.created);
  assert(!mesh_session::has_opera_secret());
  assert(mesh_session::pairing_state() == mesh_pairing::State::IDLE && g_outs.empty());

  /* The handler cannot persist (NVS refused): still nothing. */
  mesh_session::set_opera_create_handler(on_opera_create);
  g_create_ok = false;
  assert(mesh_session::submit_request(create));
  mesh_session::process(11);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::NOT_PERSISTED && !res.created);
  assert(g_create_calls == 1 && !mesh_session::has_opera_secret());
  assert(mesh_session::pairing_state() == mesh_pairing::State::IDLE && g_outs.empty());

  /* Mesh off: refused before a secret exists. */
  mesh_session::Request off = make_request(mesh_session::RequestType::SET_ENABLED);
  off.enabled = false;
  assert(mesh_session::submit_request(off));
  mesh_session::process(12);
  assert(mesh_session::take_request_result(&res));
  g_create_ok = true;
  assert(mesh_session::submit_request(create));
  mesh_session::process(13);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::MESH_DISABLED && g_create_calls == 1);
  mesh_session::Request on = make_request(mesh_session::RequestType::SET_ENABLED);
  on.enabled = true;
  assert(mesh_session::submit_request(on));
  mesh_session::process(14);
  assert(mesh_session::take_request_result(&res) && res.enabled);

  /* Created: the persisted secret IS the opera (its opera_id derives from
   * it), under canary-wap's default name, and the initiator pairing runs. */
  assert(mesh_session::submit_request(create));
  mesh_session::process(20);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && res.created);
  assert(g_create_calls == 2 && g_created_secret.size() == 32);
  bool all_zero = true;
  for (uint8_t b : g_created_secret) all_zero = all_zero && b == 0;
  assert(!all_zero);
  assert(mesh_session::has_opera_secret());
  uint8_t id[mesh_crypto::OPERA_ID_LEN], want[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(id));
  mesh_crypto::compute_opera_id(g_created_secret.data(), want);
  assert(std::memcmp(id, want, sizeof(id)) == 0);
  assert(g_created_name == mesh_session::DEFAULT_OPERA_NAME);
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(name, sizeof(name));
  assert(std::strcmp(name, "My Canary Opera") == 0);
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_INITIATOR);

  /* The joiner receives that very secret (a pure joiner, over the air). */
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x04, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x04, 0x02};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, j_pub, j_priv, 30);
  const std::vector<uint8_t> disc = wire(a);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  const std::vector<uint8_t> offer = last_to(mac_j);
  feed_pure(cj, me, offer, 31, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(mesh_session::confirm_pairing_code(32));
  const std::vector<uint8_t> conf_i = last_to(mac_j);
  a = mesh_pairing::confirm_code(cj, 32);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  const std::vector<uint8_t> complete = last_to(mac_j);
  feed_pure(cj, me, conf_i, 33, &a);
  feed_pure(cj, me, complete, 33, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  uint8_t got[32];
  assert(mesh_pairing::consume_opera_secret(cj, got));
  assert(std::memcmp(got, g_created_secret.data(), 32) == 0);
  mesh_session::process(34);

  /* An opera the session holds is never replaced — a join that landed
   * after the handler looked, or a secret NVS would not give back. */
  assert(mesh_session::submit_request(create));
  mesh_session::process(40);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OPERA_EXISTS && !res.created);
  assert(g_create_calls == 2);
  assert(mesh_session::get_opera_id(id) && std::memcmp(id, want, sizeof(id)) == 0);

  /* A pairing in progress (joining, no opera yet): refused before a secret
   * exists. */
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_opera_create_handler(on_opera_create);
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_JOIN)));
  mesh_session::process(50);
  assert(mesh_session::take_request_result(&res) &&
         res.status == mesh_session::RequestStatus::OK);
  assert(mesh_session::submit_request(create));
  mesh_session::process(51);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REFUSED && !res.created);
  assert(g_create_calls == 2 && !mesh_session::has_opera_secret());
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
  /* deinit() drops the handler: a fresh session creates nothing until the
   * integration layer installs one again. */
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv));
  assert(mesh_session::start());
  assert(mesh_session::submit_request(create));
  mesh_session::process(60);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::NOT_PERSISTED && g_create_calls == 2);
  std::printf("PASS test_rest_pair_start_creates_opera\n");
}

void test_rest_pairing_requests() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xE1 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_failed_callback(on_failed_abandoning);
  mesh_session::set_opera_name("Hearth");
  mesh_session::RequestResult res;

  /* Nothing runs until the main loop's process(). */
  mesh_session::Request join = make_request(mesh_session::RequestType::PAIR_JOIN);
  assert(mesh_session::submit_request(join));
  assert(mesh_session::pairing_state() == mesh_pairing::State::IDLE);
  mesh_session::process(10);
  assert(mesh_session::take_request_result(&res));
  assert(res.type == mesh_session::RequestType::PAIR_JOIN);
  assert(res.status == mesh_session::RequestStatus::OK);
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);

  /* A second start while one runs is refused by the state machine. */
  assert(mesh_session::submit_request(join));
  mesh_session::process(11);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REFUSED);
  /* So is a confirm with no code on screen. */
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CONFIRM)));
  mesh_session::process(12);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REFUSED);

  /* Cancel runs on the main loop and fires the FailedCallback there. */
  g_failed_fired = false;
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CANCEL)));
  assert(!g_failed_fired);
  mesh_session::process(13);
  assert(g_failed_fired);
  assert(mesh_session::take_request_result(&res));
  assert(res.type == mesh_session::RequestType::PAIR_CANCEL &&
         res.status == mesh_session::RequestStatus::OK);

  /* PAIR_START carries the secret and uses the opera's own name. */
  mesh_session::Request start = make_request(mesh_session::RequestType::PAIR_START);
  std::memcpy(start.opera_secret, S, sizeof(S));
  g_outs.clear();
  assert(mesh_session::submit_request(start));
  mesh_session::process(20);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK);
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_INITIATOR);
  assert(g_outs.size() == 1);
  mesh_pairing::PairDiscoverPayload disc;
  std::memcpy(&disc, g_outs[0].bytes.data() + 1, sizeof(disc));
  assert(std::strcmp(disc.device_name, "Hearth") == 0);
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(name, sizeof(name));
  assert(std::strcmp(name, "Hearth") == 0);

  /* A late completion never answers the next request: the handler gives
   * up while the CANCEL runs; its result is dropped, and the next request
   * gets its own. */
  g_abandon_in_failed = true;
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CANCEL)));
  mesh_session::process(21);
  g_abandon_in_failed = false;
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);   /* it did run */
  assert(!mesh_session::take_request_result(&res));                     /* result dropped */
  assert(mesh_session::submit_request(join));
  mesh_session::process(22);
  assert(mesh_session::take_request_result(&res));
  assert(res.type == mesh_session::RequestType::PAIR_JOIN &&
         res.status == mesh_session::RequestStatus::OK);

  /* A request withdrawn before the main loop took it never runs. */
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CANCEL)));
  assert(mesh_session::withdraw_request());
  mesh_session::process(23);
  assert(!mesh_session::take_request_result(&res));
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);

  /* While the mesh is off: start / join / confirm say MESH_DISABLED (the
   * disable canceled the running pairing). */
  mesh_session::Request off = make_request(mesh_session::RequestType::SET_ENABLED);
  off.enabled = false;
  assert(mesh_session::submit_request(off));
  mesh_session::process(30);
  assert(mesh_session::take_request_result(&res) && !res.enabled);
  const mesh_session::RequestType gated[] = {mesh_session::RequestType::PAIR_START,
                                             mesh_session::RequestType::PAIR_JOIN,
                                             mesh_session::RequestType::PAIR_CONFIRM};
  for (mesh_session::RequestType t : gated) {
    mesh_session::Request r = make_request(t);
    std::memcpy(r.opera_secret, S, sizeof(S));
    assert(mesh_session::submit_request(r));
    mesh_session::process(31);
    assert(mesh_session::take_request_result(&res));
    assert(res.status == mesh_session::RequestStatus::MESH_DISABLED);
  }
  assert(mesh_session::pairing_state() != mesh_pairing::State::DISCOVERING_INITIATOR);
  mesh_session::Request on = make_request(mesh_session::RequestType::SET_ENABLED);
  on.enabled = true;
  assert(mesh_session::submit_request(on));
  mesh_session::process(32);
  assert(mesh_session::take_request_result(&res) && res.enabled);

  /* While a rotation runs: start / join say REKEY_IN_FLIGHT. */
  uint8_t b_pub[32], b_priv[32], x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_session::register_trusted_peer(b_pub));
  assert(mesh_session::register_trusted_peer(x_pub));
  mesh_session::Request rm = make_request(mesh_session::RequestType::REMOVE);
  mesh_crypto::compute_fingerprint(x_pub, rm.fp);
  assert(mesh_session::submit_request(rm));
  mesh_session::process(40);
  assert(mesh_session::take_request_result(&res));
  assert(res.remove == mesh_session::RemoveResult::STARTED);
  assert(mesh_session::submit_request(start));
  mesh_session::process(41);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT);
  assert(mesh_session::submit_request(join));
  mesh_session::process(42);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT);
  std::printf("PASS test_rest_pairing_requests\n");
}

/* ── F33 part 6 — the revocation deny-list in the session ─────────────── */

void test_revocation_deny_list() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xF1 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_peer_revoked_handler(on_peer_revoked);
  g_revoked.clear();
  mesh_session::process(1000);

  uint8_t x_pub[32], x_priv[32], x_fp[8], b_pub[32], b_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
  mesh_crypto::compute_fingerprint(x_pub, x_fp);
  assert(mesh_session::register_trusted_peer(x_pub));
  assert(mesh_session::register_trusted_peer(b_pub));

  /* Removing X deny-lists it and tells the integration layer (with X's
   * pubkey, for NVS). */
  uint8_t removed[32];
  assert(mesh_session::remove_peer(x_fp, 1000, removed) == mesh_session::RemoveResult::STARTED);
  assert(mesh_session::is_revoked(x_fp) && mesh_session::revoked_count() == 1);
  assert(g_revoked.size() == 1 && g_revoked[0].pub.size() == 32 &&
         std::memcmp(g_revoked[0].pub.data(), x_pub, 32) == 0);

  /* X is not trusted again inside the grace. */
  assert(!mesh_session::register_trusted_peer(x_pub));

  /* Pairing refuses it: X's DISCOVER(JOIN) reaches nothing while we
   * initiate (after the rotation, which pairing may not overlap). */
  mesh_session::process(1000 + mesh_rekey::REKEY_TIMEOUT_MS);   /* rotation commits */
  assert(!mesh_session::rekey_in_progress());
  mesh_pairing::PairingContext cx;
  mesh_pairing::context_init(cx);
  const std::vector<uint8_t> disc_x = wire(mesh_pairing::start_joiner(cx, x_pub, x_priv, 10));
  uint8_t S2[32];
  std::memcpy(S2, S, 32);   /* the value does not matter to the refusal */
  assert(mesh_session::start_pairing_initiator(S2, "Home", 1000 + mesh_rekey::REKEY_TIMEOUT_MS));
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x00, 0x06, 0x01};
  g_outs.clear();
  const uint32_t d0 = dropped_no_peer();
  mesh_transport::test::inject_recv(mac_x, disc_x.data(), disc_x.size(), -40);
  mesh_transport::process();
  assert(g_outs.empty());                              /* no OFFER to X */
  assert(dropped_no_peer() == d0 + 1);                 /* refused, counted */
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_INITIATOR);
  /* A device that is not deny-listed still pairs. */
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  const std::vector<uint8_t> disc_j = wire(mesh_pairing::start_joiner(cj, j_pub, j_priv, 10));
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x06, 0x02};
  mesh_transport::test::inject_recv(mac_j, disc_j.data(), disc_j.size(), -40);
  mesh_transport::process();
  assert(!g_outs.empty() && std::memcmp(g_outs.back().mac, mac_j, 6) == 0);
  mesh_session::cancel_pairing();

  /* As a joiner: an OFFER from a deny-listed initiator is refused too. */
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  mesh_pairing::start_initiator(ci, x_pub, x_priv, S, "Evil", 10);
  assert(mesh_session::start_pairing_joiner(2000 + mesh_rekey::REKEY_TIMEOUT_MS));
  uint8_t me_pub[32];
  std::memcpy(me_pub, pub, 32);
  mesh_pairing::PairDiscoverPayload my_disc{};
  std::memcpy(my_disc.pubkey, me_pub, 32);
  my_disc.role = mesh_pairing::ROLE_JOINER;
  mesh_pairing::Action off = mesh_pairing::receive(ci, mac_j, mesh_pairing::MsgType::DISCOVER,
                                                   reinterpret_cast<const uint8_t*>(&my_disc),
                                                   sizeof(my_disc), 20);
  assert(off.type == mesh_pairing::ActionType::SEND_OFFER);
  const std::vector<uint8_t> offer_x = wire(off);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_x, offer_x.data(), offer_x.size(), -40);
  mesh_transport::process();
  assert(g_outs.empty());                              /* no ACCEPT to X */
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
  mesh_session::cancel_pairing();

  /* Persistence: encode → a fresh session → restore; the grace left counts
   * from the restore. */
  uint8_t blob[mesh_revocation::BLOB_MAX];
  const size_t n = mesh_session::encode_revocations(blob, sizeof(blob));
  assert(n == mesh_revocation::ENTRY_LEN);
  stand_up_session(S, pub, priv);
  assert(mesh_session::revoked_count() == 0);
  assert(mesh_session::restore_revocations(blob, n));
  assert(mesh_session::is_revoked(x_fp));
  assert(!mesh_session::register_trusted_peer(x_pub));
  assert(!mesh_session::restore_revocations(blob, n - 1));   /* malformed */

  /* After the grace it may be trusted — and paired — again. */
  mesh_session::process(mesh_revocation::REVOCATION_GRACE_MS);
  assert(!mesh_session::is_revoked(x_fp) && mesh_session::revoked_count() == 0);
  assert(mesh_session::register_trusted_peer(x_pub));
  std::printf("PASS test_revocation_deny_list\n");
}

/* A verified OFFER's removal holds even while this device runs a rotation
 * of its own: the named peer is forgotten and gets no SECRET from us; a
 * preceding OFFER inside our settle window makes us yield, and our own
 * removal is then announced again once the winner's rotation is over. */
void test_concurrent_offer_propagates_and_yields() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x07 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);   /* this device: A */
  mesh_session::set_peer_revoked_handler(on_peer_revoked);
  mesh_session::set_rekey_commit_handler(on_rekey_commit);
  g_revoked.clear();
  g_commits.clear();
  uint8_t fp_a[8];
  mesh_crypto::compute_fingerprint(pub, fp_a);

  /* Peers: W (another initiator), X (A removes it), Y (W removes it). Keep
   * generating W until it PRECEDES A, so A must yield. */
  uint8_t w_pub[32], w_priv[32], fp_w[8];
  do {
    assert(mesh_crypto::ed25519_generate_keypair(w_pub, w_priv));
    mesh_crypto::compute_fingerprint(w_pub, fp_w);
  } while (std::memcmp(fp_w, fp_a, 8) >= 0);
  uint8_t x_pub[32], x_priv[32], fp_x[8], y_pub[32], y_priv[32], fp_y[8];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  assert(mesh_crypto::ed25519_generate_keypair(y_pub, y_priv));
  mesh_crypto::compute_fingerprint(x_pub, fp_x);
  mesh_crypto::compute_fingerprint(y_pub, fp_y);
  const uint8_t mac_w[6] = {0x24, 0x0A, 0xC4, 0x00, 0x07, 0x01};
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x00, 0x07, 0x02};
  const uint8_t mac_y[6] = {0x24, 0x0A, 0xC4, 0x00, 0x07, 0x03};
  assert(mesh_session::register_trusted_peer(w_pub) && mesh_session::bind_peer_mac(fp_w, mac_w));
  assert(mesh_session::register_trusted_peer(x_pub) && mesh_session::bind_peer_mac(fp_x, mac_x));
  assert(mesh_session::register_trusted_peer(y_pub) && mesh_session::bind_peer_mac(fp_y, mac_y));

  /* A removes X: its OFFER goes to W and Y. */
  uint8_t removed[32];
  mesh_session::process(100);
  assert(mesh_session::remove_peer(fp_x, 100, removed) == mesh_session::RemoveResult::STARTED);
  /* Y answers A's OFFER; A holds it (settle). */
  g_outs.clear();
  mesh_session::process(200);

  /* W's concurrent rotation: W removes Y; its OFFER reaches A inside A's
   * settle window. */
  mesh_rekey::Context cw;
  mesh_rekey::context_init(cw);
  const uint8_t w_surv[2][8] = {{fp_a[0], fp_a[1], fp_a[2], fp_a[3], fp_a[4], fp_a[5], fp_a[6], fp_a[7]},
                                {fp_x[0], fp_x[1], fp_x[2], fp_x[3], fp_x[4], fp_x[5], fp_x[6], fp_x[7]}};
  mesh_rekey::Action w_offer = mesh_rekey::start(cw, fp_w, fp_y, w_surv, 2, 0xABCD, 150);
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t flen = build_signed_session_frame(w_pub, w_priv, S, 1, mesh_envelope::MsgType::REKEY_OFFER,
                                           w_offer.payload, w_offer.payload_len, frame, sizeof(frame));
  g_outs.clear();
  g_revoked.clear();
  inject_from(mac_w, frame, flen);
  /* Y is deny-listed and forgotten here now (W's removal holds), its radio
   * MAC gone... */
  assert(mesh_session::is_revoked(fp_y));
  assert(!mesh_transport::has_peer(mac_y));
  assert(g_revoked.size() == 1 && std::memcmp(g_revoked[0].fp.data(), fp_y, 8) == 0);
  /* ...and A yielded: it answers W's OFFER with an ACCEPT as a survivor. */
  assert(g_outs.size() == 1 && std::memcmp(g_outs[0].mac, mac_w, 6) == 0);
  mesh_envelope::Header hdr;
  const uint8_t* pl = nullptr;
  size_t plen = 0;
  assert(parse_session_frame(g_outs[0].bytes, pub, &hdr, &pl, &plen));
  assert(hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACCEPT));
  assert(mesh_session::rekey_in_progress());   /* as W's survivor */

  /* W finishes with A (W never heard A's OFFER, so X is still W's
   * survivor on W's side — the reason for the re-announce). */
  mesh_rekey::Action w_sec = mesh_rekey::receive(cw, fp_w, mesh_rekey::MsgType::ACCEPT, fp_a,
                                                 pl, plen, 150 + mesh_rekey::REKEY_SETTLE_MS);
  assert(w_sec.type == mesh_rekey::ActionType::SEND_SECRET);
  /* W resends its OFFER (it does every REKEY_RETRY_MS until the SECRET).
   * Y is already deny-listed and forgotten, so the resend reaches the
   * integration layer no second time: each callback writes NVS and logs a
   * health row, and a removal would otherwise log one per resend. */
  flen = build_signed_session_frame(w_pub, w_priv, S, 2, mesh_envelope::MsgType::REKEY_OFFER,
                                    w_offer.payload, w_offer.payload_len, frame, sizeof(frame));
  inject_from(mac_w, frame, flen);
  assert(mesh_session::is_revoked(fp_y));
  assert(g_revoked.size() == 1);
  flen = build_signed_session_frame(w_pub, w_priv, S, 3, mesh_envelope::MsgType::REKEY_SECRET,
                                    w_sec.payload, w_sec.payload_len, frame, sizeof(frame));
  g_outs.clear();
  mesh_session::process(150 + mesh_rekey::REKEY_SETTLE_MS);
  inject_from(mac_w, frame, flen);
  /* A installed W's secret (ACK out, under the old id)... */
  assert(g_commits.size() == 1);
  uint8_t got_id[mesh_crypto::OPERA_ID_LEN], old_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(got_id));
  mesh_crypto::compute_opera_id(S, old_id);
  assert(std::memcmp(got_id, old_id, sizeof(old_id)) != 0);
  /* ...and, idle again, re-announces its own removal of X: a new OFFER
   * naming X, to W. */
  g_outs.clear();
  mesh_session::process(150 + mesh_rekey::REKEY_SETTLE_MS + 50);
  assert(mesh_session::rekey_in_progress());
  bool offer_seen = false;
  for (const auto& o : g_outs) {
    if (parse_session_frame(o.bytes, pub, &hdr, &pl, &plen) &&
        hdr.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_OFFER)) {
      assert(std::memcmp(pl + mesh_rekey::REKEY_ID_LEN + mesh_rekey::EPH_LEN, fp_x, 8) == 0);
      assert(std::memcmp(hdr.opera_id, got_id, sizeof(got_id)) == 0);   /* under W's secret */
      assert(std::memcmp(o.mac, mac_w, 6) == 0);
      offer_seen = true;
    }
  }
  assert(offer_seen);
  std::printf("PASS test_concurrent_offer_propagates_and_yields\n");
}

int main() {
  std::srand(0xC51F0);
  test_start_initiator_emits_discover_init();
  test_start_joiner_emits_discover_join();
  test_joiner_offer_surfaces_code_with_accept();
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
  /* v0.4 — the registry's outer frame (spec §4.5; crypto review pending). */
  test_outer_frame_is_the_registry_frame();
  test_leave_opera();
  test_peer_left_dispatch();
  /* Review fix — replay tombstones survive leave / re-pair / reboot. */
  test_replay_tombstones_across_leave_and_repair();
  test_replay_tombstones_keep_age_across_reboot();
  test_leave_keeps_outbound_counter();
  test_build_mesh_alerts_json();
  test_build_mesh_status_json_disabled();
  test_rest_buffers_fit_worst_case();
  /* F10-rekey — remove_peer + rotation (CRYPTO: maintainer review). */
  test_rekey_session_as_initiator();
  test_rekey_session_as_survivor();
  test_rekey_refusals_and_forgeries();
  test_rekey_timeout_and_no_survivors();
  /* Review fix — a verified frame speaks only for its signer, once. */
  test_verified_frame_speaks_only_for_its_signer();
  test_rekey_frames_speak_only_for_their_signer_once();
  /* Review fix — the F10 REST mutators run on the main loop. */
  test_rest_request_slot();
  test_parse_fingerprint_hex();
  /* F33 part 1 — the transport peer table on the device. */
  test_bound_peer_is_heard_and_reached();
  test_removed_peer_leaves_transport_table();
  /* F49 part 3 withdrawn — a frame from a new address moves no binding. */
  test_bound_peer_new_address_is_dropped_not_learned();
  test_unheard_broadcast_replayed_from_a_new_address_moves_nothing();
  test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing();
  test_pairing_over_the_air_as_initiator();
  test_pairing_over_the_air_as_joiner();
  test_pairing_over_the_air_joiner_confirms_first();
  test_pairing_over_the_air_initiator_confirms_first();
  test_pairing_over_the_air_with_a_pre_f97_joiner();
  test_pairing_over_the_air_with_a_pre_f97_initiator();
  test_failed_pairing_removes_partner_address();
  test_repair_moves_a_trusted_peers_address();
  test_paired_peer_bound_reports_the_bind();
  test_refused_repair_bind_is_not_persisted_across_reboot();
  test_successful_repair_is_persisted_across_reboot();
  /* F119, F120 — the boot restore of the stored addresses. */
  test_stored_mac_must_drop_truth_table();
  test_boot_restore_binds_neither_member_of_a_shared_address();
  test_boot_restore_drops_entries_of_peers_no_longer_trusted();
  /* F70 */
  test_pair_contact_replay_records_nothing_and_gets_no_accept();
  test_copied_member_address_moves_no_link();
  test_forgetting_a_peer_drops_only_its_own_address();
  test_unbound_member_frame_is_never_taken();
  test_opera_sends_with_only_a_pairing_partner_reach_nobody();
  test_opera_sends_reach_bound_members_only();
  test_opera_sends_count_only_what_the_transport_took();
  /* F118 — a pairing whose partner this device cannot hold fails. */
  test_a_partner_the_initiator_cannot_hold_fails_the_pairing();
  test_the_initiator_asks_again_before_it_seals();
  test_a_joiner_that_cannot_hold_its_initiator_fails_the_pairing();
  /* F33 part 3 — the outbound counter survives a reboot. */
  test_outbound_counter_reserve_ahead();
  test_outbound_counter_without_reservation_restarts();
  /* F33 part 5 — the pairing routes run on the main loop. */
  test_rest_pairing_requests();
  test_rest_pair_start_creates_opera();
  /* F33 part 6 — the revocation deny-list; concurrent removals. */
  test_revocation_deny_list();
  test_concurrent_offer_propagates_and_yields();
  std::printf("\nALL MESH_SESSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
