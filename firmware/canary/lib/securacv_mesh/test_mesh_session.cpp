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
 *   6. cancel_pairing() fires the FailedCallback and wipes state; on a
 *      pairing that already ended it does nothing (F135).
 *   7. As the initiator, a lost COMPLETE goes again every 2 s until the
 *      member is heard, and not once it is no longer a member bound where
 *      it paired from or this device no longer holds that opera (F134).
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

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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
int                      g_failed_count = 0;   /* FailedCallbacks since reset_world (F135) */
void on_failed(mesh_pairing::FailReason why, const uint8_t* fp) {
  g_failed_fired = true;
  ++g_failed_count;
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
  g_failed_count = 0;
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

/* F162: a signed HEARTBEAT as it goes on the air: the version byte, type
 * 16, no payload, the signature. A session sends one to every bound member
 * right after NOTIFY_PAIRED and once after a boot restore with members. */
bool is_heartbeat(const std::vector<uint8_t>& f) {
  return f.size() == mesh_envelope::MIN_FRAME_LEN &&
         f[mesh_envelope::OFFSET_VERSION] == mesh_envelope::OPERA_VERSION &&
         f[mesh_envelope::OFFSET_MSG_TYPE] ==
             static_cast<uint8_t>(mesh_envelope::MsgType::HEARTBEAT);
}

/* F324: one heartbeat as a receiver takes it: where it went, its counter,
 * and the signer's process() clock when it was signed (header timestamp). */
struct Beat {
  uint8_t  mac[6];
  uint64_t counter;
  uint32_t timestamp;
};

/* F324: what a running session sends on its own while nothing else
 * happens, since it heartbeats on a timer: its periodic HEARTBEAT, and only
 * that. Every frame in `outs` must be an empty signed HEARTBEAT that
 * verifies under `pub`, carries `opera_id` and goes to one of the n_bound
 * addresses in `bound`; to each address the counters strictly increase,
 * and one round is one signed frame (a repeated counter repeats the bytes).
 * A heartbeat to a pairing partner or an unbound member (F101), one signed
 * by another key or under another opera, and any other frame fail here, so
 * a silence check that admits heartbeats admits nothing else. Returns them
 * in order. */
std::vector<Beat> assert_only_member_heartbeats(const std::vector<OutFrame>& outs,
                                                const uint8_t pub[mesh_crypto::PUBKEY_LEN],
                                                const uint8_t opera_id[mesh_crypto::OPERA_ID_LEN],
                                                const uint8_t (*bound)[6], size_t n_bound) {
  std::vector<Beat> beats;
  const OutFrame* prev = nullptr;
  for (const auto& o : outs) {
    assert(is_heartbeat(o.bytes));
    mesh_envelope::Header h;
    const uint8_t* payload = nullptr;
    size_t payload_len = 99;
    assert(mesh_envelope::parse_and_verify(o.bytes.data(), o.bytes.size(), pub, &h, &payload,
                                           &payload_len));
    assert(payload_len == 0);
    assert(h.msg_type == static_cast<uint8_t>(mesh_envelope::MsgType::HEARTBEAT));
    assert(std::memcmp(h.opera_id, opera_id, mesh_crypto::OPERA_ID_LEN) == 0);
    bool to_member = false;
    for (size_t b = 0; b < n_bound && !to_member; ++b) {
      to_member = std::memcmp(o.mac, bound[b], 6) == 0;
    }
    assert(to_member);
    for (auto it = beats.rbegin(); it != beats.rend(); ++it) {
      if (std::memcmp(it->mac, o.mac, 6) == 0) {
        assert(h.counter > it->counter);
        break;
      }
    }
    if (!beats.empty()) {
      assert(h.counter >= beats.back().counter);
      if (h.counter == beats.back().counter) assert(o.bytes == prev->bytes);
    }
    Beat b;
    std::memcpy(b.mac, o.mac, 6);
    b.counter   = h.counter;
    b.timestamp = h.timestamp;
    beats.push_back(b);
    prev = &o;
  }
  return beats;
}

/* F324's cadence as the docs state it: spec §4.5's registry row and §7.1,
 * THREAT_MODEL and the U1 bench row say every 30 s +/- 3 s. Every spacing
 * check below derives its bounds from the two constants, so a check that
 * reads them cannot catch a change to them: a jitter of 0 (one fixed
 * deadline, the lock-step cadence the jitter exists to prevent) or a 40 s
 * interval passed every one of them. This pins the numbers the docs give;
 * mesh_session.h's own asserts keep the jitter above 0 and two of the
 * longest intervals short of STALE. */
static_assert(mesh_session::HEARTBEAT_INTERVAL_MS == 30000 &&
                  mesh_session::HEARTBEAT_JITTER_MS == 3000,
              "spec §4.5 and §7.1: a member heartbeat every 30 s +/- 3 s");
/* The same for the first-counter save's second half (review round 4): the
 * threat model, spec §8.3 and main.cpp say 66 s, and the test that times it
 * reads the constant, so it cannot catch a change to it. Its lost-heartbeat
 * leg times the member's frames from the heartbeat constants instead, which
 * catches a deadline too short to outlast one lost heartbeat; this pins the
 * rest, a longer deadline being a longer window for the reboot the docs
 * name. */
static_assert(mesh_session::FIRST_COUNTER_RESAVE_MS == 66000,
              "THREAT_MODEL, spec §8.3: the second first-counter save 66 s after the first");

/* F324's spacing, from the constants, never typed: a heartbeat is due
 * HEARTBEAT_INTERVAL_MS +/- HEARTBEAT_JITTER_MS after the one before and
 * goes out on the first pass at or past that, so with passes `pass_ms`
 * apart two in a row are kBeatGapMin to beat_gap_max(pass_ms) apart. */
constexpr uint32_t kBeatGapMin =
    mesh_session::HEARTBEAT_INTERVAL_MS - mesh_session::HEARTBEAT_JITTER_MS;
constexpr uint32_t beat_gap_max(uint32_t pass_ms) {
  return mesh_session::HEARTBEAT_INTERVAL_MS + mesh_session::HEARTBEAT_JITTER_MS + pass_ms;
}
constexpr uint32_t ceil_div(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

/* The heartbeat times `times`, after one at `prev`, over the passes of
 * [w0, w1] `pass_ms` apart: each gap, the first from `prev`, within the
 * bounds above; none missing at the end (w1 - last < the longest gap); and
 * their number between what all-longest and all-shortest gaps give,
 * derived here. Returns that range in *n_min, *n_max for the PASS line.
 * Every comparison is an unsigned difference, so a window that spans the
 * 32-bit clock's wrap reads the same as any other. */
void check_beat_spacing(const std::vector<uint32_t>& times, uint32_t prev, uint32_t w0,
                        uint32_t w1, uint32_t pass_ms, size_t* n_min, size_t* n_max) {
  const uint32_t lo = kBeatGapMin, hi = beat_gap_max(pass_ms);
  uint32_t last = prev;
  for (uint32_t t : times) {
    assert(t - w0 <= w1 - w0);   /* in [w0, w1] */
    assert(t - last >= lo && t - last <= hi);
    last = t;
  }
  assert(w1 - last < hi);
  *n_min = (size_t)((w1 - prev) / hi) - (size_t)ceil_div(w0 - prev, lo) + 1;
  *n_max = (size_t)((w1 - prev) / lo) - (size_t)ceil_div(w0 - prev, hi) + 1;
  assert(times.size() >= *n_min && times.size() <= *n_max);
}

/* The times of the beats in `beats` that went to `mac`. */
std::vector<uint32_t> beat_times_to(const std::vector<Beat>& beats, const uint8_t mac[6]) {
  std::vector<uint32_t> v;
  for (const auto& b : beats) {
    if (std::memcmp(b.mac, mac, 6) == 0) v.push_back(b.timestamp);
  }
  return v;
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
  g_failed_fp.assign(8, 0xEE);
  mesh_session::cancel_pairing();
  assert(g_failed_fired);
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  /* F118's FailedCallback: the reason, and no partner — no DISCOVER or
   * OFFER named one yet, so the fingerprint is nullptr, not a fingerprint
   * of an all-zero key or of whatever the stack held. */
  assert(g_failed_why == mesh_pairing::FailReason::CANCELED);
  assert(g_failed_fp.empty());
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
      /*peers_total=*/3, /*peers_online=*/2, /*alerts=*/7, /*code=*/123456, /*last_pairing=*/nullptr));

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
      mesh_pairing::State::AWAITING_CONFIRM, 1, 0, 0, /*code=*/42, /*last_pairing=*/nullptr));
  assert(std::strstr(buf, "\"state\":\"PAIRING_CONFIRM\"") != nullptr);
  assert(std::strstr(buf, "\"pairing_code\":42") != nullptr);

  /* PAIRING_INIT → NO code (early-leak guard). */
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, true, nullptr, "X",
      mesh_pairing::State::DISCOVERING_INITIATOR, 1, 0, 0, /*code=*/42, /*last_pairing=*/nullptr));
  assert(std::strstr(buf, "\"state\":\"PAIRING_INIT\"") != nullptr);
  assert(std::strstr(buf, "pairing_code") == nullptr);
  std::printf("PASS test_build_mesh_status_json_pairing_code_only_in_confirm\n");
}

void test_build_mesh_status_json_escapes_name() {
  char buf[512];
  /* A name with a quote must not break the JSON envelope. */
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, false, nullptr, "Evil\"name",
      mesh_pairing::State::IDLE, 0, 0, 0, 0, /*last_pairing=*/nullptr));
  assert(std::strstr(buf, "\"opera_name\":\"Evil\\\"name\"") != nullptr);
  std::printf("PASS test_build_mesh_status_json_escapes_name\n");
}

void test_build_mesh_status_json_no_opera_empty_id() {
  char buf[512];
  assert(mesh_api::build_mesh_status_json(
      buf, sizeof(buf), true, false, nullptr, "",
      mesh_pairing::State::IDLE, 0, 0, 0, 0, /*last_pairing=*/nullptr));
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
      mesh_pairing::State::IDLE, 0, 0, 0, 0, /*last_pairing=*/nullptr));
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
  assert(mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 1);
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
  assert(mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 2);
  assert(recs[0].kind == mesh_alert::Kind::TEMP_DRIFT && recs[0].timestamp_ms == 5000);
  assert(recs[1].kind == mesh_alert::Kind::CAMERA_TAMPER);
  /* cap is honored. */
  assert(mesh_session::read_alerts(recs, 1) == 1);
  assert(recs[0].kind == mesh_alert::Kind::TEMP_DRIFT);

  /* DELETE semantics: history empties, the lifetime counters stay. */
  mesh_session::clear_alerts();
  assert(mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 0);
  assert(mesh_session::alerts_received() == 2);
  assert(mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS) == 1);
  assert(links[0].alerts_received == 2);

  /* deinit wipes counters and history. */
  mesh_session::deinit();
  assert(mesh_session::alerts_received() == 0);
  assert(mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY) == 0);
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
  const size_t n = mesh_session::read_alerts(recs, sizeof(recs) / sizeof(recs[0]));
  assert(n == mesh_session::MAX_ALERT_HISTORY);
  for (size_t k = 0; k < n; ++k) {
    assert(recs[k].witness_seq == (uint32_t)(1000 + total - k));
  }
  /* F197: the history is a loop_snapshot::Log, which copies in storage
   * order, and after the wrap storage starts mid-history (slots 0..2 hold
   * the three newest). The route still answers newest first, and a cap
   * below what is held keeps the newest ones, not the first slots. */
  mesh_alert::Record few[5];
  assert(mesh_session::read_alerts(few, 1) == 1);
  assert(few[0].witness_seq == (uint32_t)(1000 + total));
  assert(mesh_session::read_alerts(few, 5) == 5);
  for (size_t k = 0; k < 5; ++k) {
    assert(few[k].witness_seq == (uint32_t)(1000 + total - k));
  }
  assert(mesh_session::read_alerts(few, 0) == 0);
  assert(mesh_session::read_alerts(nullptr, 5) == 0);
  /* A clear, then two more: only those two, newest first. */
  mesh_session::clear_alerts();
  for (size_t i = total + 1; i <= total + 2; ++i) {
    const size_t flen = build_alert_frame(tx_pub, tx_priv, secret, i,
                                          mesh_alert::Kind::TEMP_DRIFT, 2,
                                          (uint32_t)(1000 + i), frame, sizeof(frame));
    inject_from(mac, frame, flen);
  }
  assert(mesh_session::read_alerts(recs, sizeof(recs) / sizeof(recs[0])) == 2);
  assert(recs[0].witness_seq == (uint32_t)(1000 + total + 2));
  assert(recs[1].witness_seq == (uint32_t)(1000 + total + 1));
  /* Leaving the opera wipes the history it holds and the counter. */
  (void)mesh_session::leave_opera(100);
  assert(mesh_session::read_alerts(recs, sizeof(recs) / sizeof(recs[0])) == 0);
  assert(mesh_session::alerts_received() == 0);
  std::printf("PASS test_alert_ring_wraps_newest_first\n");
}

/* F197: GET /api/mesh/alerts reads the history on the httpd task while the
 * main loop's receive path stores alerts and a DELETE (clear_alerts(), run
 * by the drain) empties it. Until F197 the route copied the ring in place,
 * so a body could hold a record half overwritten by a newer alert, run out
 * of order across a store, or straddle a clear. Every read must be whole
 * records, newest first and all from one moment: each record's fields agree
 * with its witness_seq (its kind, severity, sender and timestamp are made
 * from it), the numbers run down one at a time, and no body mixes the two
 * sides of a clear (each clear moves the numbers on by 1000, so a straddle
 * shows a gap). */
static mesh_alert::Kind kind_for(uint32_t w) {
  static const mesh_alert::Kind kinds[3] = {mesh_alert::Kind::ENCLOSURE_TAMPER,
                                            mesh_alert::Kind::TEMP_DRIFT,
                                            mesh_alert::Kind::CAMERA_TAMPER};
  return kinds[w % 3];
}

void test_alert_reads_stay_whole_while_the_main_loop_stores_and_clears() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x5A + i);
  uint8_t rx_pub[mesh_crypto::PUBKEY_LEN], rx_priv[mesh_crypto::PRIVKEY_LEN];
  stand_up_session(secret, rx_pub, rx_priv);
  uint8_t tx_pub[mesh_crypto::PUBKEY_LEN], tx_priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(tx_pub, tx_priv));
  assert(mesh_session::register_trusted_peer(tx_pub));
  uint8_t tx_fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(tx_pub, tx_fp);
  const uint8_t mac[6] = {0x02, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5};
  assert(mesh_session::bind_peer_mac(tx_fp, mac));

  std::atomic<bool> done(false);
  std::atomic<long> reads(0), torn(0), full(0);
  std::thread reader([&] {
    mesh_alert::Record recs[mesh_session::MAX_ALERT_HISTORY + 4];
    while (!done.load(std::memory_order_acquire)) {
      const size_t n = mesh_session::read_alerts(recs, sizeof(recs) / sizeof(recs[0]));
      bool ok = n <= mesh_session::MAX_ALERT_HISTORY;
      for (size_t k = 0; ok && k < n; ++k) {
        const uint32_t w = recs[k].witness_seq;
        ok = std::memcmp(recs[k].sender_fp, tx_fp, sizeof(tx_fp)) == 0 &&
             recs[k].kind == kind_for(w) && recs[k].severity == (uint8_t)(w % 8) &&
             recs[k].timestamp_ms == w &&
             (k == 0 || w + 1 == recs[k - 1].witness_seq);
      }
      if (!ok) torn.fetch_add(1);
      if (n == mesh_session::MAX_ALERT_HISTORY) full.fetch_add(1);
      reads.fetch_add(1);
    }
  });
  const int steps = 1200;
  uint32_t w = 0;
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  for (int step = 1; step <= steps; ++step) {
    if (step % 40 == 0) {
      mesh_session::clear_alerts();   /* what the drain runs for a DELETE */
      w += 1000;
    }
    ++w;
    mesh_session::process(w);         /* the record is stamped with this clock */
    const size_t flen = build_alert_frame(tx_pub, tx_priv, secret, (uint64_t)step,
                                          kind_for(w), (uint8_t)(w % 8), w,
                                          frame, sizeof(frame));
    inject_from(mac, frame, flen);
    if (g_outs.size() > 64) g_outs.clear();
  }
  done.store(true, std::memory_order_release);
  reader.join();
  assert(mesh_session::alerts_received() == (uint32_t)steps);
  assert(reads.load() > 0 && full.load() > 0);
  /* deinit() wipes the history it holds (the next session starts empty). */
  mesh_alert::Record after[mesh_session::MAX_ALERT_HISTORY];
  assert(mesh_session::read_alerts(after, mesh_session::MAX_ALERT_HISTORY) > 0);
  mesh_session::deinit();
  assert(mesh_session::read_alerts(after, mesh_session::MAX_ALERT_HISTORY) == 0);
  if (torn.load() != 0) {
    std::printf("FAIL: %ld of %ld alert reads were torn, out of order or straddled a clear\n",
                torn.load(), reads.load());
    std::fflush(stdout);
  }
  assert(torn.load() == 0);
  std::printf("PASS test_alert_reads_stay_whole_while_the_main_loop_stores_and_clears "
              "(%ld reads)\n", reads.load());
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
      nullptr, "", mesh_session::pairing_state(), 1, 1, 0, 0, /*last_pairing=*/nullptr));
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
      mesh_pairing::State::IDLE, 2, 2, 5, 0, /*last_pairing=*/nullptr));
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

/* GET /api/mesh's body, as handle_mesh_status builds it (defined below,
 * with the F133 tests). */
std::string status_body();
bool has(const std::string& body, const char* frag);

/* F94, through the session as the INITIATOR: its own CONFIRM, sent back to
 * it from the joiner's address, ends the pairing (bad_confirm in GET
 * /api/mesh), and nothing is sealed, sent, registered or bound: the partner's
 * address leaves the transport table and the joiner's genuine CONFIRM,
 * after it, gets no COMPLETE. Until F94 the session sent the COMPLETE,
 * reported PAIRED and registered a member whose owner never confirmed. */
void test_a_reflected_confirm_fails_the_pairing_through_the_session() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x94 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  assert(mesh_session::set_opera_secret(S));
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x94, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x94, 0x02};
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  mesh_pairing::context_init(cj);
  const std::vector<uint8_t> disc = wire(mesh_pairing::start_joiner(cj, j_pub, j_priv, 10));

  assert(mesh_session::start_pairing_initiator(S, "Home", 20));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, disc.data(), disc.size(), -40);
  mesh_transport::process();
  mesh_pairing::Action a;
  feed_pure(cj, me, last_to(mac_j), 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  const std::vector<uint8_t> accept = wire(a);
  mesh_transport::test::inject_recv(mac_j, accept.data(), accept.size(), -40);
  mesh_transport::process();
  assert(g_code_ready != 0 && g_code_ready == cj.confirmation_code);

  assert(mesh_session::confirm_pairing_code(40));
  const std::vector<uint8_t> conf_i = last_to(mac_j);
  assert(!conf_i.empty() &&
         conf_i[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, conf_i.data(), conf_i.size(), -40);
  mesh_transport::process();
  mesh_session::process(50);
  assert(g_failed_fired && g_failed_count == 1);
  assert(g_failed_why == mesh_pairing::FailReason::BAD_CONFIRM);
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::BAD_CONFIRM);
  assert(mesh_session::pairing_outcome() == mesh_pairing::Outcome::FAILED);
  assert(has(status_body(), "\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"bad_confirm\""));
  assert(!g_paired_fired && g_paired_mac.empty());
  assert(mesh_session::trusted_peer_count() == 0);
  assert(!transport_has(mac_j));
  for (const OutFrame& f : g_outs) {
    assert(f.bytes.empty() ||
           f.bytes[0] != static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE));
  }

  /* The joiner's owner confirms after it: its CONFIRM finds no pairing. */
  a = mesh_pairing::confirm_code(cj, 60);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  for (uint32_t t = 70; t < 70 + 3 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 500) {
    mesh_session::process(t);
  }
  assert(g_outs.empty());
  assert(!g_paired_fired && mesh_session::trusted_peer_count() == 0);
  assert(g_failed_count == 1);
  std::printf("PASS test_a_reflected_confirm_fails_the_pairing_through_the_session\n");
}

/* The same as the JOINER: its own CONFIRM, sent back to it from the
 * initiator's address, ends its pairing (bad_confirm in GET /api/mesh), and
 * nothing is opened, registered or bound: the initiator's address leaves the
 * transport table, so the COMPLETE the initiator sends on the joiner's
 * genuine CONFIRM is dropped there. That initiator still completes, holding
 * the genuine CONFIRM: a joiner's reflected CONFIRM ends only the joiner's
 * pairing, as a wrong hash in that slot already did (spec §5.2). Until F94
 * the joiner took its own CONFIRM as the initiator's, and that COMPLETE
 * paired it. */
void test_a_reflected_confirm_fails_the_joiners_pairing_through_the_session() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x95 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  g_paired_mac.clear();

  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x00, 0x95, 0x01};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x00, 0x95, 0x02};
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t i_pub[32], i_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 10);

  assert(mesh_session::start_pairing_joiner(20));
  feed_pure(ci, me, last_to(bcast), 30, &a);
  assert(a.type == mesh_pairing::ActionType::SEND_OFFER);
  const std::vector<uint8_t> offer = wire(a);
  mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
  mesh_transport::process();
  feed_pure(ci, me, last_to(mac_i), 40, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  assert(a.confirmation_code == mesh_session::pairing_confirmation_code());

  /* This owner confirms; the CONFIRM comes back from the initiator's address. */
  assert(mesh_session::confirm_pairing_code(50));
  const std::vector<uint8_t> conf_j = last_to(mac_i);
  assert(!conf_j.empty() &&
         conf_j[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM));
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_i, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  mesh_session::process(55);
  assert(g_failed_fired && g_failed_count == 1);
  assert(g_failed_why == mesh_pairing::FailReason::BAD_CONFIRM);
  assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
  assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::BAD_CONFIRM);
  assert(mesh_session::pairing_outcome() == mesh_pairing::Outcome::FAILED);
  assert(has(status_body(), "\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"bad_confirm\""));
  assert(!g_paired_fired && g_paired_mac.empty());
  assert(!transport_has(mac_i));

  /* The genuine CONFIRM reaches the initiator, which completes; its
   * CONFIRM and COMPLETE, and a copy, reach a joiner with no pairing. */
  feed_pure(ci, me, conf_j, 60, &a);
  assert(a.type == mesh_pairing::ActionType::NONE && ci.peer_confirmed);
  a = mesh_pairing::confirm_code(ci, 70);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  inject_all(mac_i, wire_all(a));
  assert(mesh_pairing::tick(ci, 75).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  a = mesh_pairing::tick(ci, 70 + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  inject_all(mac_i, wire_all(a));
  for (uint32_t t = 80; t < 80 + 3 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 500) {
    mesh_session::process(t);
  }
  assert(!g_paired_fired && !g_paired_with_secret && g_paired_mac.empty());
  assert(!transport_has(mac_i) && mesh_session::trusted_peer_count() == 0);
  assert(g_failed_count == 1);
  assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::BAD_CONFIRM);
  std::printf("PASS test_a_reflected_confirm_fails_the_joiners_pairing_through_the_session\n");
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
  /* Since F162 the same pass's NOTIFY_PAIRED sends the new member one
   * heartbeat, after the two pairing frames. */
  assert(g_outs.size() == 3);
  assert(std::memcmp(g_outs[0].mac, mac_j, 6) == 0);
  assert(std::memcmp(g_outs[1].mac, mac_j, 6) == 0);
  assert(std::memcmp(g_outs[2].mac, mac_j, 6) == 0);
  /* The initiator's own CONFIRM, not the joiner's bytes back: since F94
   * each names its sender's role. */
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM));
  assert(g_outs[0].bytes.size() == conf_j.size() && g_outs[0].bytes != conf_j);
  assert(g_outs[1].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE));
  assert(is_heartbeat(g_outs[2].bytes));
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
    /* Nothing more, but the joiner's periodic heartbeat (F324): to the
     * initiator, its one member, under the opera it just joined. */
    g_outs.clear();
    for (uint32_t t = T0 + 20000; t < T0 + 40000; t += 500) mesh_session::process(t);
    uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
    mesh_crypto::compute_opera_id(S, opera_id);
    const uint8_t member[1][6] = {{mac_i[0], mac_i[1], mac_i[2], mac_i[3], mac_i[4], mac_i[5]}};
    assert_only_member_heartbeats(g_outs, pub, opera_id, member, 1);
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
  /* The COMPLETE went with the confirm; since F162 the NOTIFY_PAIRED below
   * sends a heartbeat after it, to a member that was bound. */
  const std::vector<uint8_t> complete = last_to(mac_j);
  mesh_session::process(t + 4);
  assert(g_paired_fired);
  feed_pure(cj, me, complete, t + 5, &a);
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
  /* F162: a pairing whose member was bound tells every bound member once
   * (J at X, and C itself). */
  auto beats_to = [](size_t from, const uint8_t to[6]) {
    size_t k = 0;
    for (size_t i = from; i < g_outs.size(); ++i) {
      if (is_heartbeat(g_outs[i].bytes) && std::memcmp(g_outs[i].mac, to, 6) == 0) ++k;
    }
    return k;
  };
  size_t outs_at = g_outs.size();
  assert(pair_from(S, me, mac_c, c_pub, c_priv, 200));
  assert(g_bound_calls.size() == 2 && g_bound_calls[1].bound);
  assert(beats_to(outs_at, mac_x) == 1 && beats_to(outs_at, mac_c) == 1);

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
  outs_at = g_outs.size();
  const uint64_t ctr_at = mesh_session::outbound_counter();
  assert(pair_from(S, me, mac_n, n_pub, n_priv, 400));
  assert(g_bound_calls.size() == 3);
  assert(!g_bound_calls[2].bound && g_bound_calls[2].after_paired_cb);
  /* F162's review: a refused bind has no new member to tell, so its
   * NOTIFY_PAIRED sends no heartbeat, not even to the members bound
   * before it, and signs nothing. */
  assert(beats_to(outs_at, mac_x) == 0 && beats_to(outs_at, mac_c) == 0);
  assert(beats_to(outs_at, mac_n) == 0);
  assert(mesh_session::outbound_counter() == ctr_at);
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

/* The fourth verdict. A member's entry that bind_peer_mac refuses on its
 * own (here a broadcast address; or no room in the transport table) is
 * REFUSED: not bound, not counted, and kept in NVS, as before F119/F120 —
 * stored_mac_must_drop keeps it, so only SHARED and UNTRUSTED entries go. */
void test_boot_restore_keeps_a_refused_entry() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x21 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  const uint8_t bc[6]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x21, 0x00, 0x0C};
  uint8_t m_pub[32], m_priv[32], m_fp[8], c_pub[32], c_priv[32], c_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(m_pub, m_priv));
  assert(mesh_crypto::ed25519_generate_keypair(c_pub, c_priv));
  mesh_crypto::compute_fingerprint(m_pub, m_fp);
  mesh_crypto::compute_fingerprint(c_pub, c_fp);
  g_nvs_pubs.emplace_back(m_pub, m_pub + 32);
  g_nvs_pubs.emplace_back(c_pub, c_pub + 32);
  raw_blob_entry(m_fp, bc);
  raw_blob_entry(c_fp, mac_c);
  const size_t len_before = g_nvs_macs_len;

  size_t bound = 99;
  std::vector<mesh_session::StoredMacVerdict> v = main_like_boot(pub, priv, S, true, &bound);
  assert(v.size() == 2 && bound == 1);
  assert(v[0] == mesh_session::StoredMacVerdict::REFUSED);
  assert(v[1] == mesh_session::StoredMacVerdict::BOUND);
  assert(g_nvs_macs_len == len_before);              /* both kept */
  assert(heard_from(c_pub, c_priv, S, 1, mac_c));
  std::printf("PASS test_boot_restore_keeps_a_refused_entry\n");
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

/* F118 — can_hold_partner itself, one refusal at a time. Each case is
 * built so that only the check it names can refuse it (a key the opera can
 * take, at an address no member holds, with room in the transport table,
 * unless the case is about that), with a control beside it that the same
 * setup admits; so the case fails if its check is removed. The pairing
 * tests below reach the held-address and full-opera refusals through the
 * protocol; the deny-list, the broadcast/group/zero address and the full
 * transport table are pinned here. */
void test_can_hold_partner_refusals() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x61 + i);
  uint8_t pub[32], priv[32];
  const uint8_t free_mac[6] = {0x24, 0x0A, 0xC4, 0x61, 0x00, 0x01};
  uint8_t n_pub[32], n_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(n_pub, n_priv));

  /* Not a unicast address: broadcast, all-zero, a group address. */
  stand_up_session(S, pub, priv);
  {
    const uint8_t bc[6]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t zero[6]  = {0, 0, 0, 0, 0, 0};
    const uint8_t group[6] = {0x01, 0x00, 0x5E, 0x61, 0x00, 0x01};
    assert(mesh_session::can_hold_partner(n_pub, free_mac));            /* control */
    assert(!mesh_session::can_hold_partner(n_pub, bc));
    assert(!mesh_session::can_hold_partner(n_pub, zero));
    assert(!mesh_session::can_hold_partner(n_pub, group));
    assert(!mesh_session::can_hold_partner(nullptr, free_mac));
    assert(!mesh_session::can_hold_partner(n_pub, nullptr));
  }

  /* A deny-listed key (§5.6), through the session's own removal: X was a
   * member, the owner removed it, so it is no longer trusted and its old
   * address is free; only the deny-list refuses it. */
  stand_up_session(S, pub, priv);
  {
    uint8_t x_pub[32], x_priv[32], x_fp[8];
    assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
    mesh_crypto::compute_fingerprint(x_pub, x_fp);
    const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x61, 0x00, 0x0A};
    assert(mesh_session::register_trusted_peer(x_pub));
    assert(mesh_session::bind_peer_mac(x_fp, mac_x));
    uint8_t removed[32] = {0};
    const mesh_session::RemoveResult r = mesh_session::remove_peer(x_fp, 0, removed);
    assert(r == mesh_session::RemoveResult::COMMITTED || r == mesh_session::RemoveResult::STARTED);
    assert(mesh_session::is_revoked(x_fp) && mesh_session::trusted_peer_count() == 0);
    assert(!transport_has(mac_x));
    assert(!mesh_session::can_hold_partner(x_pub, free_mac));
    assert(!mesh_session::can_hold_partner(x_pub, mac_x));
    assert(mesh_session::can_hold_partner(n_pub, free_mac));            /* control */
    assert(mesh_session::can_hold_partner(n_pub, mac_x));               /* control */
  }

  /* A full transport table: eight addresses in it, none a member's. A new
   * address has no room; one already in the table needs none. */
  stand_up_session(S, pub, priv);
  {
    uint8_t m[6] = {0x24, 0x0A, 0xC4, 0x61, 0x01, 0x00};
    while (mesh_transport::peer_count() < mesh_transport::MESH_TRANSPORT_MAX_PEERS) {
      ++m[5];
      assert(mesh_transport::add_peer(m));
    }
    assert(!transport_has(free_mac));
    assert(!mesh_session::can_hold_partner(n_pub, free_mac));
    assert(mesh_session::can_hold_partner(n_pub, m));                   /* control */
  }

  /* A full opera: eight members, none bound. A new key is refused at a
   * free address; a member re-pairing from it needs no slot. */
  stand_up_session(S, pub, priv);
  {
    uint8_t first_pub[32] = {0};
    for (size_t k = 0; k < mesh_session::MAX_TRUSTED_PEERS; ++k) {
      uint8_t k_pub[32], k_priv[32];
      assert(mesh_crypto::ed25519_generate_keypair(k_pub, k_priv));
      assert(mesh_session::register_trusted_peer(k_pub));
      if (k == 0) std::memcpy(first_pub, k_pub, 32);
    }
    assert(!mesh_session::can_hold_partner(n_pub, free_mac));
    assert(mesh_session::can_hold_partner(first_pub, free_mac));        /* control */
  }

  /* An address another member is bound to: refused to a new key and to
   * another member; the member it is bound to is admitted there. */
  stand_up_session(S, pub, priv);
  {
    uint8_t a_pub[32], a_priv[32], a_fp[8], b_pub[32], b_priv[32];
    assert(mesh_crypto::ed25519_generate_keypair(a_pub, a_priv));
    assert(mesh_crypto::ed25519_generate_keypair(b_pub, b_priv));
    mesh_crypto::compute_fingerprint(a_pub, a_fp);
    const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x61, 0x00, 0x0A};
    assert(mesh_session::register_trusted_peer(a_pub));
    assert(mesh_session::register_trusted_peer(b_pub));
    assert(mesh_session::bind_peer_mac(a_fp, mac_a));
    assert(!mesh_session::can_hold_partner(n_pub, mac_a));
    assert(!mesh_session::can_hold_partner(b_pub, mac_a));
    assert(mesh_session::can_hold_partner(a_pub, mac_a));               /* control */
    assert(mesh_session::can_hold_partner(a_pub, free_mac));            /* control */
    assert(mesh_session::can_hold_partner(b_pub, free_mac));            /* control */
  }
  std::printf("PASS test_can_hold_partner_refusals\n");
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

/* ── F134 — the initiator sends a lost COMPLETE again ────────────────────
 *
 * mesh_pairing keeps the frames it sent and its tick sends them again every
 * 2 s; the session ends the copies when the member is heard, when it is no
 * longer a member bound where it paired from, and when this device no
 * longer holds the opera the COMPLETE carried. */

/* This session as the INITIATOR at loop time T0, the joiner's owner first,
 * its own owner's confirm through the REST slot at T0 + 10: the CONFIRM
 * and the COMPLETE go to mac_j and NOTIFY_PAIRED runs in the same pass,
 * which since F162 sends one heartbeat to every bound member, the joiner
 * among them when it was bound. Returns the two pairing frames; nothing is delivered to the
 * joiner. */
std::vector<std::vector<uint8_t>> initiator_completes(const uint8_t S[32], const uint8_t me[6],
                                                      const uint8_t mac_j[6],
                                                      const uint8_t j_pub[32],
                                                      const uint8_t j_priv[32], uint32_t T0,
                                                      mesh_pairing::PairingContext& cj) {
  mesh_session::process(T0);
  initiator_to_code(S, me, mac_j, j_pub, j_priv, T0, cj);
  mesh_pairing::Action a = mesh_pairing::confirm_code(cj, T0 + 5);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  g_outs.clear();
  assert(rest_confirm(T0 + 10) == mesh_session::RequestStatus::OK);
  assert(g_paired_fired);
  std::vector<std::vector<uint8_t>> sent;
  size_t heartbeats_to_j = 0;
  for (const auto& o : g_outs) {
    if (is_heartbeat(o.bytes)) {   /* F162 */
      if (std::memcmp(o.mac, mac_j, 6) == 0) ++heartbeats_to_j;
      continue;
    }
    assert(std::memcmp(o.mac, mac_j, 6) == 0);
    sent.push_back(o.bytes);
  }
  /* F162: one to the joiner when the pairing bound it, none when not. */
  assert(!g_bound_calls.empty());
  assert(heartbeats_to_j == (g_bound_calls.back().bound ? 1u : 0u));
  assert(sent.size() == 2);
  assert(sent[0][0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_CONFIRM));
  assert(sent[1][0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE));
  g_outs.clear();
  return sent;
}

/* Through the session, both frames of the COMPLETE lost: nothing goes to
 * the joiner for 2 s, then exactly the same two frames, to it alone; the
 * joiner opens the copy. The copies go on (nothing heard it yet: the pure
 * joiner here sends nothing, not even the heartbeat a session would), a
 * verified frame from another member does not end them, and the joiner's
 * own first verified frame does: nothing more for the rest of the window
 * but this side's periodic heartbeats to its two members (F324). Fails on
 * the code before F134 (no copy), and with the heard check removed from
 * on_opera_frame (copies to the window's end). */
void test_a_lost_complete_is_sent_again_through_the_session() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x34 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  mesh_session::set_tamper_alert_handler(on_alert_rx);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x40, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x40, 0x02};
  const uint8_t mac_k[6] = {0x24, 0x0A, 0xC4, 0x13, 0x40, 0x0B};
  uint8_t k_pub[32], k_priv[32], k_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(k_pub, k_priv));
  assert(mesh_session::register_trusted_peer(k_pub));
  mesh_crypto::compute_fingerprint(k_pub, k_fp);
  assert(mesh_session::bind_peer_mac(k_fp, mac_k));
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  const uint32_t T0 = 200000, T = T0 + 10;
  const auto first = initiator_completes(S, me, mac_j, j_pub, j_priv, T0, cj);
  assert(cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);   /* got nothing */

  for (uint32_t t = T; t < T + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 100) {
    mesh_session::process(t);
  }
  assert(g_outs.empty());
  mesh_session::process(T + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
  assert(g_outs.size() == 2 && sent_to(mac_j) == 2);
  assert(g_outs[0].bytes == first[0] && g_outs[1].bytes == first[1]);
  mesh_pairing::Action a;
  feed_pure(cj, me, g_outs[0].bytes, T + 2000, &a);
  feed_pure(cj, me, g_outs[1].bytes, T + 2000, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  uint8_t got[32];
  assert(mesh_pairing::consume_opera_secret(cj, got));
  assert(std::memcmp(got, S, sizeof(S)) == 0);

  /* Frames that reach a completed initiator move nothing and bring no
   * copy early: its own CONFIRM reflected (since F94 not the joiner's
   * bytes, and a completed pairing reads no CONFIRM anyway) and its
   * COMPLETE replayed, from the joiner's address; the same and a joiner's
   * DISCOVER from a third radio (dropped at the transport: no pairing
   * runs). */
  g_outs.clear();
  const uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x13, 0x40, 0x0F};
  mesh_pairing::PairingContext cx;
  mesh_pairing::context_init(cx);
  const std::vector<uint8_t> disc_x = wire(mesh_pairing::start_joiner(cx, k_pub, k_priv, T + 2100));
  for (const uint8_t* from : {mac_j, mac_x}) {
    mesh_transport::test::inject_recv(from, first[0].data(), first[0].size(), -40);
    mesh_transport::test::inject_recv(from, first[1].data(), first[1].size(), -40);
    mesh_transport::test::inject_recv(from, disc_x.data(), disc_x.size(), -40);
    mesh_transport::process();
  }
  mesh_session::process(T + 2 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS - 1);
  assert(g_outs.empty());
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);

  /* Not heard yet: the next copy comes. Another member's frame changes
   * nothing. */
  g_outs.clear();
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t n = build_alert_frame(k_pub, k_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 7,
                               frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_k, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 1);
  mesh_session::process(T + 2 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
  assert(sent_to(mac_j) == 2);

  /* The joiner's first frame: no more copies, to the window's end. */
  n = build_alert_frame(j_pub, j_priv, S, 1, mesh_alert::Kind::TEMP_DRIFT, 3, 8,
                        frame, sizeof(frame));
  mesh_transport::test::inject_recv(mac_j, frame, n, -40);
  mesh_transport::process();
  assert(g_alerts_rx.size() == 2);
  g_outs.clear();
  for (uint32_t t = T + 4100; t < T + mesh_pairing::COMPLETE_RESEND_WINDOW_MS + 5000; t += 500) {
    mesh_session::process(t);
  }
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t members[2][6] = {{mac_k[0], mac_k[1], mac_k[2], mac_k[3], mac_k[4], mac_k[5]},
                                 {mac_j[0], mac_j[1], mac_j[2], mac_j[3], mac_j[4], mac_j[5]}};
  assert_only_member_heartbeats(g_outs, pub, opera_id, members, 2);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(g_failed_count == 0 && g_bound_calls.size() == 1);
  std::printf("PASS test_a_lost_complete_is_sent_again_through_the_session\n");
}

/* The copies end, with no copy sent, as soon as they can no longer help:
 *   0  control — nothing changes, and a copy goes out at 2 s;
 *   1  the member is removed (unregistered: a LEAVE, a removal, a rotation
 *      that dropped it all end that way);
 *   2  this device leaves the opera (REST LEAVE);
 *   3  the opera's secret changes (a rotation's commit installs a new one);
 *   4  the member's bind failed (its PairedCallback registered nothing);
 *   5  a new pairing starts (its own frames only);
 *   6  nobody hears it: (WINDOW - 1) / INTERVAL copies, then none;
 *   7  the member is bound at another address since (the copies would go
 *      where it no longer is).
 * Fails on the code before F134 (no copy in case 0), and with the session's
 * check removed (cases 1-4 send copies). */
void test_complete_copies_stop_when_they_can_no_longer_help() {
  uint8_t S[32], S2[32];
  for (size_t i = 0; i < sizeof(S); ++i) { S[i] = (uint8_t)(0x44 + i); S2[i] = (uint8_t)(0x54 + i); }
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x41, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x41, 0x02};
  const uint8_t mac_m[6] = {0x24, 0x0A, 0xC4, 0x13, 0x41, 0x0E};
  for (int c = 0; c <= 7; ++c) {
    uint8_t pub[32], priv[32];
    stand_up_session(S, pub, priv);
    reset_fake_main_nvs();
    mesh_session::set_paired_callback(c == 4 ? main_like_paired_unregistered : main_like_paired);
    mesh_session::set_paired_peer_bound_callback(main_like_bound);
    uint8_t j_pub[32], j_priv[32], j_fp[8];
    assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
    mesh_crypto::compute_fingerprint(j_pub, j_fp);
    mesh_pairing::PairingContext cj;
    const uint32_t T0 = 300000, T = T0 + 10;
    initiator_completes(S, me, mac_j, j_pub, j_priv, T0, cj);
    switch (c) {
      case 1: assert(mesh_session::unregister_trusted_peer(j_fp)); break;
      case 2: {
        assert(mesh_session::submit_request(make_request(mesh_session::RequestType::LEAVE)));
        mesh_session::process(T + 100);
        mesh_session::RequestResult res;
        assert(mesh_session::take_request_result(&res));
        assert(res.status == mesh_session::RequestStatus::OK);
        break;
      }
      case 3: assert(mesh_session::set_opera_secret(S2)); break;
      case 4: assert(g_bound_calls.size() == 1 && !g_bound_calls[0].bound); break;
      case 5: assert(mesh_session::start_pairing_joiner(T + 100)); break;
      case 7: assert(mesh_session::bind_peer_mac(j_fp, mac_m)); break;
      default: break;
    }
    g_outs.clear();
    size_t copies = 0;
    const uint32_t until = c == 6 ? T + mesh_pairing::COMPLETE_RESEND_WINDOW_MS + 10000
                                  : T + 3 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS + 50;
    for (uint32_t t = T + 200; t < until; t += 100) {
      mesh_transport::test::set_now_ms(t);   /* the storm limiter's clock (100 sends/s) */
      mesh_session::process(t);
    }
    for (const auto& o : g_outs) {
      if (std::memcmp(o.mac, mac_j, 6) == 0 &&
          o.bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE)) {
        ++copies;
      }
    }
    if (c == 0) {
      assert(copies == 3 && sent_to(mac_j) == 6);
    } else if (c == 6) {
      assert(copies == (mesh_pairing::COMPLETE_RESEND_WINDOW_MS - 1) /
                       mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
    } else {
      assert(sent_to(mac_j) == 0);
      if (c == 5) {   /* the new pairing's DISCOVER, nothing else */
        assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
      } else {
        assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
      }
    }
  }
  std::printf("PASS test_complete_copies_stop_when_they_can_no_longer_help"
              "  (removed, left, rotated, unbound, new pairing, window, moved)\n");
}

/* As the JOINER: copies of the COMPLETE reaching a session already PAIRED
 * do nothing — no second PairedCallback or bind, nothing sent, no failure. */
void test_complete_copies_reaching_a_paired_joiner_do_nothing() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x64 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x42, 0x01};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x13, 0x42, 0x02};
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
  assert(mesh_session::confirm_pairing_code(50));
  feed_pure(ci, me, last_to(mac_i), 60, &a);
  a = mesh_pairing::confirm_code(ci, 70);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  inject_all(mac_i, wire_all(a));
  assert(g_paired_fired && g_paired_with_secret && g_bound_calls.size() == 1);
  g_outs.clear();
  for (uint32_t t = 2070; t < 12070; t += 2000) {
    mesh_session::process(t);
    mesh_pairing::Action copy = mesh_pairing::tick(ci, t);
    if (copy.type == mesh_pairing::ActionType::SEND_COMPLETE) inject_all(mac_i, wire_all(copy));
  }
  assert(ci.complete_copies >= 4);
  assert(g_outs.empty() && g_bound_calls.size() == 1 && g_failed_count == 0);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  std::printf("PASS test_complete_copies_reaching_a_paired_joiner_do_nothing\n");
}

/* The COMPLETE copies to `to` in g_outs. */
size_t complete_copies_to(const uint8_t to[6]) {
  size_t n = 0;
  for (const auto& o : g_outs) {
    n += std::memcmp(o.mac, to, 6) == 0 &&
         o.bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE);
  }
  return n;
}

/* Only the joiner's own verified, fresh frame, from the address it paired
 * from and under this opera, ends the copies: the heard stop sits after
 * every check on_opera_frame makes. Each of these frames fails one of them,
 * is dropped, and leaves the next two copies due:
 *   0  the joiner's fingerprint under a bad signature, from its own address
 *      (its key went out in clear in its DISCOVER and the fingerprint is in
 *      every header, so any radio can write this frame);
 *   1  its genuine frame, from another member's address (F70);
 *   2  its genuine frame, signed under another opera;
 *   3  a replay of a frame it sent before this pairing (a member that
 *      re-pairs keeps its counter);
 *   4  control: a fresh genuine frame from its own address ends them.
 * With the stop moved above the address check, the signature check, the
 * opera check or the counter check, one forged or stale frame ends the
 * copies and case 0, 1, 2 or 3 fails. */
void test_only_the_joiners_own_fresh_frame_ends_the_copies() {
  uint8_t S[32], S2[32];
  for (size_t i = 0; i < sizeof(S); ++i) { S[i] = (uint8_t)(0xC1 + i); S2[i] = (uint8_t)(0xD1 + i); }
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x43, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x43, 0x02};
  const uint8_t mac_k[6] = {0x24, 0x0A, 0xC4, 0x13, 0x43, 0x0B};
  for (int c = 0; c <= 4; ++c) {
    uint8_t pub[32], priv[32];
    stand_up_session(S, pub, priv);
    reset_fake_main_nvs();
    mesh_session::set_paired_callback(main_like_paired);
    mesh_session::set_paired_peer_bound_callback(main_like_bound);
    mesh_session::set_tamper_alert_handler(on_alert_rx);
    uint8_t k_pub[32], k_priv[32], k_fp[8];   /* another member, bound at mac_k */
    assert(mesh_crypto::ed25519_generate_keypair(k_pub, k_priv));
    assert(mesh_session::register_trusted_peer(k_pub));
    mesh_crypto::compute_fingerprint(k_pub, k_fp);
    assert(mesh_session::bind_peer_mac(k_fp, mac_k));
    uint8_t j_pub[32], j_priv[32], j_fp[8];
    assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
    mesh_crypto::compute_fingerprint(j_pub, j_fp);
    uint8_t before[mesh_envelope::MAX_FRAME_LEN];
    size_t before_n = 0;
    if (c == 3) {   /* a member, heard at counter 7, that pairs again */
      assert(mesh_session::register_trusted_peer(j_pub));
      assert(mesh_session::bind_peer_mac(j_fp, mac_j));
      before_n = build_alert_frame(j_pub, j_priv, S, 7, mesh_alert::Kind::TEMP_DRIFT, 3, 1,
                                   before, sizeof(before));
      mesh_transport::test::inject_recv(mac_j, before, before_n, -40);
      mesh_transport::process();
      assert(g_alerts_rx.size() == 1);
      g_alerts_rx.clear();
    }
    mesh_pairing::PairingContext cj;
    const uint32_t T0 = 900000, T = T0 + 10;
    initiator_completes(S, me, mac_j, j_pub, j_priv, T0, cj);

    uint8_t f[mesh_envelope::MAX_FRAME_LEN];
    size_t n = 0;
    const uint8_t* from = mac_j;
    switch (c) {
      case 0:
        n = build_alert_frame(j_pub, j_priv, S, 50, mesh_alert::Kind::TEMP_DRIFT, 3, 2, f, sizeof(f));
        f[n - 1] ^= 0x01;   /* the signature's last byte */
        break;
      case 1:
        n = build_alert_frame(j_pub, j_priv, S, 50, mesh_alert::Kind::TEMP_DRIFT, 3, 2, f, sizeof(f));
        from = mac_k;
        break;
      case 2:
        n = build_alert_frame(j_pub, j_priv, S2, 50, mesh_alert::Kind::TEMP_DRIFT, 3, 2, f, sizeof(f));
        break;
      case 3:
        std::memcpy(f, before, before_n);
        n = before_n;
        break;
      default:
        n = build_alert_frame(j_pub, j_priv, S, 50, mesh_alert::Kind::TEMP_DRIFT, 3, 2, f, sizeof(f));
        break;
    }
    mesh_transport::test::inject_recv(from, f, n, -40);
    mesh_transport::process();
    assert(g_alerts_rx.size() == (c == 4 ? 1u : 0u));   /* 0-3 dropped, 4 taken */
    g_outs.clear();
    for (uint32_t t = T + 100; t <= T + 2 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 100) {
      mesh_transport::test::set_now_ms(t);
      mesh_session::process(t);
    }
    if (c == 4) {
      assert(g_outs.empty());
    } else {
      assert(complete_copies_to(mac_j) == 2 && sent_to(mac_j) == 4 && g_outs.size() == 4);
    }
    assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  }
  std::printf("PASS test_only_the_joiners_own_fresh_frame_ends_the_copies"
              "  (bad signature, another address, another opera, replay; control)\n");
}

/* process() runs the session's check before the pairing tick, which is what
 * sends a copy, so a REST LEAVE, or a REST REMOVE of the member, drained in
 * the very pass a copy falls due sends no copy: not to a member just
 * removed, and not with the secret of an opera just left or rotated. With
 * the check after the tick, each sends one. */
void test_a_leave_or_removal_in_a_copys_pass_sends_no_copy() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xE1 + i);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x44, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x44, 0x02};
  for (int remove = 0; remove <= 1; ++remove) {
    uint8_t pub[32], priv[32];
    stand_up_session(S, pub, priv);
    reset_fake_main_nvs();
    mesh_session::set_paired_callback(main_like_paired);
    mesh_session::set_paired_peer_bound_callback(main_like_bound);
    uint8_t j_pub[32], j_priv[32], j_fp[8];
    assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
    mesh_crypto::compute_fingerprint(j_pub, j_fp);
    mesh_pairing::PairingContext cj;
    const uint32_t T0 = 950000, T = T0 + 10;
    initiator_completes(S, me, mac_j, j_pub, j_priv, T0, cj);
    for (uint32_t t = T + 100; t < T + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 100) {
      mesh_transport::test::set_now_ms(t);
      mesh_session::process(t);
    }
    assert(g_outs.empty());
    mesh_session::Request r = make_request(remove ? mesh_session::RequestType::REMOVE
                                                  : mesh_session::RequestType::LEAVE);
    if (remove) std::memcpy(r.fp, j_fp, sizeof(j_fp));
    assert(mesh_session::submit_request(r));
    mesh_transport::test::set_now_ms(T + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
    mesh_session::process(T + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);   /* a copy is due */
    mesh_session::RequestResult res;
    assert(mesh_session::take_request_result(&res));
    assert(res.status == mesh_session::RequestStatus::OK);
    if (remove) assert(res.remove == mesh_session::RemoveResult::COMMITTED);
    assert(complete_copies_to(mac_j) == 0);
    for (uint32_t t = T + 2100; t <= T + 3 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 100) {
      mesh_transport::test::set_now_ms(t);
      mesh_session::process(t);
    }
    assert(complete_copies_to(mac_j) == 0);   /* and none after it */
  }
  std::printf("PASS test_a_leave_or_removal_in_a_copys_pass_sends_no_copy  (LEAVE, REMOVE)\n");
}

/* ── F133 — GET /api/mesh reports the last pairing's outcome ─────────────
 *
 * The web UI's pairing poll read only `state`, and an initiator already in
 * an opera returns to ACTIVE or CONNECTING after a timeout, a refusal or a
 * cancel exactly as after a success, so the page said the pairing was
 * complete. The body now carries pairing_seq, pairing_result and
 * pairing_fail_reason, after every older field. */

/* PAIR_CANCEL through the REST slot, as the handler sends it. */
mesh_session::RequestStatus rest_cancel(uint32_t now) {
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_CANCEL)));
  mesh_session::process(now);
  mesh_session::RequestResult res;
  assert(mesh_session::take_request_result(&res));
  return res.status;
}

/* GET /api/mesh's body, built the way securacv_network.cpp's
 * handle_mesh_status builds it since F161: from the view the main loop last
 * published (read_status), never from the live state. */
std::string status_body() {
  mesh_session::StatusView view;
  mesh_session::read_status(&view);
  char body[mesh_api::STATUS_JSON_CAP];
  assert(mesh_api::build_mesh_status_json_from_view(body, sizeof(body), view));
  return body;
}

bool has(const std::string& body, const char* frag) { return body.find(frag) != std::string::npos; }

/* The first "last_seen_sec" in a peers body (F324's ages). */
unsigned long last_seen_sec_of(const std::string& body) {
  const char* key = "\"last_seen_sec\":";
  const size_t at = body.find(key);
  assert(at != std::string::npos);
  return std::strtoul(body.c_str() + at + std::strlen(key), nullptr, 10);
}

/* The builder: the three fields come after every older one, so the body
 * without them is the old body byte for byte (a page that reads only the
 * old fields parses it as before); the fail reason is "none" unless the
 * result is "failed"; with no report the body is the old one. */
void test_build_mesh_status_json_reports_the_last_pairing() {
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  for (size_t i = 0; i < sizeof(opera_id); ++i) opera_id[i] = (uint8_t)(0x30 + i);
  char old_body[mesh_api::STATUS_JSON_CAP], body[mesh_api::STATUS_JSON_CAP];
  assert(mesh_api::build_mesh_status_json(old_body, sizeof(old_body), true, true, opera_id,
                                          "Home", mesh_pairing::State::FAILED, 2, 1, 0, 0, /*last_pairing=*/nullptr));
  assert(std::strstr(old_body, "pairing_") == nullptr);
  struct Case { mesh_pairing::Outcome o; mesh_pairing::FailReason r; const char* tail; };
  const Case cases[] = {
    {mesh_pairing::Outcome::FAILED, mesh_pairing::FailReason::TIMEOUT,
     ",\"pairing_seq\":3,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"timeout\"}"},
    {mesh_pairing::Outcome::FAILED, mesh_pairing::FailReason::PARTNER_REFUSED,
     ",\"pairing_seq\":3,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"partner_refused\"}"},
    {mesh_pairing::Outcome::FAILED, mesh_pairing::FailReason::CANCELED,
     ",\"pairing_seq\":3,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"canceled\"}"},
    /* A reason the outcome does not carry is not reported. */
    {mesh_pairing::Outcome::PAIRED, mesh_pairing::FailReason::TIMEOUT,
     ",\"pairing_seq\":3,\"pairing_result\":\"paired\",\"pairing_fail_reason\":\"none\"}"},
    {mesh_pairing::Outcome::RUNNING, mesh_pairing::FailReason::NONE,
     ",\"pairing_seq\":3,\"pairing_result\":\"running\",\"pairing_fail_reason\":\"none\"}"},
    {mesh_pairing::Outcome::NONE, mesh_pairing::FailReason::NONE,
     ",\"pairing_seq\":3,\"pairing_result\":\"none\",\"pairing_fail_reason\":\"none\"}"},
  };
  for (const Case& c : cases) {
    const mesh_api::PairingReport rep{3, c.o, c.r};
    assert(mesh_api::build_mesh_status_json(body, sizeof(body), true, true, opera_id, "Home",
                                            mesh_pairing::State::FAILED, 2, 1, 0, 0, &rep));
    const std::string want = std::string(old_body, std::strlen(old_body) - 1) + c.tail;
    assert(want == body);
  }
  std::printf("PASS test_build_mesh_status_json_reports_the_last_pairing\n");
}

/* STATUS_JSON_CAP (the handler's buffer) holds the widest body: a 32-byte
 * opera name of control bytes (six bytes each, escaped), the pairing code,
 * every number at its widest and the longest result and reason. */
void test_status_json_fits_worst_case() {
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  std::memset(opera_id, 0xFF, sizeof(opera_id));
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  std::memset(name, 0x01, mesh_pairing::MAX_OPERA_NAME_LEN);
  name[mesh_pairing::MAX_OPERA_NAME_LEN] = '\0';
  const mesh_api::PairingReport rep{0xFFFFFFFFu, mesh_pairing::Outcome::FAILED,
                                    mesh_pairing::FailReason::PARTNER_REFUSED};
  char body[mesh_api::STATUS_JSON_CAP];
  assert(mesh_api::build_mesh_status_json(body, sizeof(body), false, true, opera_id, name,
                                          mesh_pairing::State::AWAITING_CONFIRM_PEER,
                                          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 999999, &rep));
  assert(has(body, "\"pairing_code\":999999") && has(body, "\"pairing_seq\":4294967295"));
  /* ...and the 512 bytes the handler allocated before F133 would not. */
  char old_buf[512];
  assert(!mesh_api::build_mesh_status_json(old_buf, sizeof(old_buf), false, true, opera_id, name,
                                           mesh_pairing::State::AWAITING_CONFIRM_PEER,
                                           0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 999999, &rep));
  std::printf("PASS test_status_json_fits_worst_case  (%zu of %zu B)\n", std::strlen(body),
              mesh_api::STATUS_JSON_CAP);
}

/* Through the session, as an initiator already in an opera (the page's
 * case: CONNECTING or ACTIVE after a failure as after a success), each
 * ending is told apart, and each pairing has its own number:
 *   1  a timeout          → CONNECTING, failed, timeout;
 *   2  a cancel (REST)    → failed, canceled; the POST answer names seq 2,
 *                           and a start and a join refused while it runs
 *                           name none and leave it at 2, running;
 *   3  a refusal at the confirm (a member holds the partner's address)
 *                         → failed, partner_refused;
 *   4  a success          → paired; while the NOTIFY_PAIRED is still due
 *                           (the COMPLETE sent in a transport pass, the
 *                           member not yet registered) → running;
 * and, as a JOINER, 5 a success → paired, and the REST join names seq 5.
 * Fails on the code before F133: the body has no such fields and the
 * state alone is CONNECTING after 1-3 as after 4. */
void test_get_mesh_tells_each_pairing_outcome() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x33 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_paired_callback(on_paired_register);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x30, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x30, 0x02};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x13, 0x30, 0x0C};
  add_bound_member(mac_c);
  mesh_session::publish_status();   /* main.cpp's setup() publish (F161) */
  std::string b = status_body();
  assert(has(b, "\"pairing_seq\":0,\"pairing_result\":\"none\",\"pairing_fail_reason\":\"none\""));
  assert(has(b, "\"state\":\"CONNECTING\""));

  /* 1: a timeout. */
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  initiator_to_code(S, me, mac_j, j_pub, j_priv, 1000, cj);
  mesh_session::process(1002);   /* the loop's session pass publishes (F161) */
  b = status_body();
  assert(has(b, "\"state\":\"PAIRING_CONFIRM\"") &&
         has(b, "\"pairing_seq\":1,\"pairing_result\":\"running\""));
  mesh_session::process(1000 + mesh_pairing::PAIRING_TIMEOUT_MS);
  b = status_body();
  assert(has(b, "\"state\":\"CONNECTING\""));
  assert(has(b, "\"pairing_seq\":1,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"timeout\""));

  /* 2: a cancel through the REST slot; the start's answer names its seq. */
  mesh_session::Request st = make_request(mesh_session::RequestType::PAIR_START);
  std::memcpy(st.opera_secret, S, sizeof(S));
  assert(mesh_session::submit_request(st));
  mesh_session::process(400000);
  mesh_session::RequestResult res;
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && res.pairing_seq == 2);
  assert(has(status_body(), "\"pairing_seq\":2,\"pairing_result\":\"running\""));
  /* A second start and a join while it runs (a second tab, a double
   * click) are refused, name no number, and leave the count alone: the
   * first page's poll still reads its own pairing, running. Since F136 the
   * join is refused as this Canary's opera (OPERA_EXISTS, before the
   * running pairing is asked); it was REFUSED. */
  for (int k = 0; k < 2; ++k) {
    mesh_session::Request again = make_request(k == 0 ? mesh_session::RequestType::PAIR_START
                                                       : mesh_session::RequestType::PAIR_JOIN);
    if (k == 0) std::memcpy(again.opera_secret, S, sizeof(S));
    assert(mesh_session::submit_request(again));
    mesh_session::process(400010 + k);
    assert(mesh_session::take_request_result(&res));
    assert(res.status == (k == 0 ? mesh_session::RequestStatus::REFUSED
                                 : mesh_session::RequestStatus::OPERA_EXISTS) &&
           res.pairing_seq == 0);
    assert(has(status_body(), "\"pairing_seq\":2,\"pairing_result\":\"running\""));
  }
  assert(mesh_session::pairing_seq() == 2);
  assert(rest_cancel(400100) == mesh_session::RequestStatus::OK);
  b = status_body();
  assert(has(b, "\"state\":\"CONNECTING\""));
  assert(has(b, "\"pairing_seq\":2,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"canceled\""));

  /* 3: refused at the confirm: the partner pairs from C's address. */
  uint8_t x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  initiator_to_code(S, me, mac_c, x_pub, x_priv, 500000, cj);
  assert(rest_confirm(500010) == mesh_session::RequestStatus::PARTNER_REFUSED);
  b = status_body();
  assert(has(b, "\"state\":\"CONNECTING\""));
  assert(has(b, "\"pairing_seq\":3,\"pairing_result\":\"failed\","
                "\"pairing_fail_reason\":\"partner_refused\""));

  /* 4: a success, the COMPLETE sent in a transport pass: running until the
   * next process() reports PAIRED and registers the member. */
  initiator_to_code(S, me, mac_j, j_pub, j_priv, 600000, cj);
  assert(rest_confirm(600010) == mesh_session::RequestStatus::OK);
  mesh_pairing::Action a = mesh_pairing::confirm_code(cj, 600020);
  const std::vector<uint8_t> conf_j = wire(a);
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(has(status_body(), "\"pairing_seq\":4,\"pairing_result\":\"running\""));
  mesh_session::process(600030);
  b = status_body();
  assert(has(b, "\"pairing_seq\":4,\"pairing_result\":\"paired\",\"pairing_fail_reason\":\"none\""));
  assert(mesh_session::trusted_peer_count() == 2);

  /* 5: as a joiner, through the REST slot. */
  uint8_t pub2[32], priv2[32];
  stand_up_session(nullptr, pub2, priv2);
  mesh_session::set_paired_callback(on_paired_register);
  assert(has(status_body(), "\"pairing_seq\":0"));
  for (int k = 0; k < 4; ++k) {   /* four earlier pairings, canceled */
    assert(mesh_session::start_pairing_joiner(100 + k));
    mesh_session::cancel_pairing();
  }
  assert(mesh_session::submit_request(make_request(mesh_session::RequestType::PAIR_JOIN)));
  mesh_session::process(1000);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && res.pairing_seq == 5);
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x13, 0x30, 0x11};
  uint8_t i_pub[32], i_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 1000);
  feed_pure(ci, me, last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), 1010, &a);
  const std::vector<uint8_t> offer = wire(a);
  mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
  mesh_transport::process();
  feed_pure(ci, me, last_to(mac_i), 1020, &a);
  assert(mesh_session::confirm_pairing_code(1030));
  feed_pure(ci, me, last_to(mac_i), 1040, &a);
  a = mesh_pairing::confirm_code(ci, 1050);
  inject_all(mac_i, wire_all(a));
  mesh_session::process(1060);   /* the loop's session pass publishes (F161) */
  b = status_body();
  assert(has(b, "\"pairing_seq\":5,\"pairing_result\":\"paired\",\"pairing_fail_reason\":\"none\""));
  /* A reboot starts the count again. */
  uint8_t pub3[32], priv3[32];
  stand_up_session(nullptr, pub3, priv3);
  assert(has(status_body(), "\"pairing_seq\":0,\"pairing_result\":\"none\""));
  std::printf("PASS test_get_mesh_tells_each_pairing_outcome\n");
}

/* ── F135 — a cancel leaves an ended pairing alone ───────────────────────
 *
 * The REST pair/cancel runs at the start of process(), before the pairing
 * tick. Until F135 mesh_pairing::cancel() failed every state but IDLE. */

/* As the INITIATOR, its owner first: the joiner's CONFIRM arrives in a
 * transport pass and the session sends its CONFIRM and the COMPLETE (PAIRED,
 * the NOTIFY_PAIRED due at the next tick). The owner's cancel lands in the
 * next process(), ahead of that tick. The pairing stays PAIRED: the
 * PairedCallback runs, the member is registered, bound and stored (the
 * main.cpp stand-in), its address stays in the table, and no
 * FailedCallback fires; the joiner opens the COMPLETE. The cancel leaves
 * F134's copies running, and so does a second one: with both frames lost,
 * the copy 2 s later is what the joiner opens. On the code before F135 the
 * cancel turned it FAILED: one FailedCallback (canceled), no
 * PairedCallback, the partner's address taken out, nothing stored, while
 * the joiner held the secret. With a cancel that ends the copies, the
 * joiner never gets one. */
void test_a_cancel_after_the_complete_still_reports_paired() {
  for (int lost = 0; lost < 2; ++lost) {   /* 1: the CONFIRM and COMPLETE lost */
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x35 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  g_in_paired_cb_done = false;
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x13, 0x50, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x13, 0x50, 0x02};
  uint8_t j_pub[32], j_priv[32], j_fp[8];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_crypto::compute_fingerprint(j_pub, j_fp);
  mesh_pairing::PairingContext cj;
  initiator_to_code(S, me, mac_j, j_pub, j_priv, 100, cj);
  assert(rest_confirm(110) == mesh_session::RequestStatus::OK);
  mesh_pairing::Action a;
  feed_pure(cj, me, last_to(mac_j), 115, &a);          /* the joiner checks it */
  a = mesh_pairing::confirm_code(cj, 120);
  const std::vector<uint8_t> conf_j = wire(a);
  g_outs.clear();
  mesh_transport::test::inject_recv(mac_j, conf_j.data(), conf_j.size(), -40);
  mesh_transport::process();                           /* the transport pass */
  assert(sent_to(mac_j) == 2);                         /* CONFIRM, COMPLETE */
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(!g_paired_fired && g_failed_count == 0);

  assert(rest_cancel(130) == mesh_session::RequestStatus::OK);
  assert(g_paired_fired && !g_paired_with_secret);
  assert(g_failed_count == 0);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(mesh_session::pairing_fail_reason() == mesh_pairing::FailReason::NONE);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(g_bound_calls.size() == 1 && g_bound_calls[0].bound);
  assert(std::memcmp(g_bound_calls[0].fp, j_fp, 8) == 0);
  assert(g_nvs_pubs.size() == 1 && std::memcmp(g_nvs_pubs[0].data(), j_pub, 32) == 0);
  assert(transport_has(mac_j));

  const std::vector<OutFrame> first = g_outs;
  if (lost) {
    /* Nothing reached the joiner. The cancel left F134's copies running,
     * so 2 s after the first send the same two frames go to it again; it
     * opens them. */
    g_outs.clear();
    for (uint32_t t = 200; t <= 120 + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS + 100; t += 100) {
      mesh_transport::test::set_now_ms(t);
      mesh_session::process(t);
    }
    assert(g_outs.size() == 2 && sent_to(mac_j) == 2);
    assert(g_outs[0].bytes == first[0].bytes && g_outs[1].bytes == first[1].bytes);
  }
  /* The joiner takes the COMPLETE: both ends agree. */
  feed_pure(cj, me, g_outs[0].bytes, 2300, &a);
  feed_pure(cj, me, g_outs[1].bytes, 2300, &a);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  uint8_t got[32];
  assert(mesh_pairing::consume_opera_secret(cj, got));
  assert(std::memcmp(got, S, sizeof(S)) == 0);

  /* Another cancel, after PAIRED was reported: still nothing, and the
   * copies still go (nothing heard the joiner): one in the next 2 s. */
  assert(rest_cancel(2400) == mesh_session::RequestStatus::OK);
  mesh_session::cancel_pairing();
  assert(g_failed_count == 0 && g_bound_calls.size() == 1);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  g_outs.clear();
  for (uint32_t t = 2500; t < 2500 + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS; t += 100) {
    mesh_transport::test::set_now_ms(t);
    mesh_session::process(t);
  }
  assert(g_outs.size() == 2 && sent_to(mac_j) == 2);
  assert(g_outs[1].bytes == first[1].bytes);
  }
  std::printf("PASS test_a_cancel_after_the_complete_still_reports_paired"
              "  (the COMPLETE delivered, and lost then sent again)\n");
}

/* A pairing that has FAILED is not failed again: a timeout, then a REST
 * cancel and a direct one, fire the FailedCallback once, and the reason
 * stays the timeout; a canceled pairing canceled again fires it once too.
 * On the code before F135 each cancel fired it again, with the reason
 * overwritten (canceled). */
void test_a_cancel_on_a_failed_pairing_fires_nothing_again() {
  for (int first = 0; first < 2; ++first) {
    reset_world();
    assert(mesh_session::start_pairing_joiner(1000));
    if (first == 0) {
      mesh_session::process(1000 + mesh_pairing::PAIRING_TIMEOUT_MS);
      assert(g_failed_count == 1 && g_failed_why == mesh_pairing::FailReason::TIMEOUT);
    } else {
      assert(rest_cancel(2000) == mesh_session::RequestStatus::OK);
      assert(g_failed_count == 1 && g_failed_why == mesh_pairing::FailReason::CANCELED);
    }
    const mesh_pairing::FailReason why = first == 0 ? mesh_pairing::FailReason::TIMEOUT
                                                    : mesh_pairing::FailReason::CANCELED;
    assert(rest_cancel(3000 + mesh_pairing::PAIRING_TIMEOUT_MS) == mesh_session::RequestStatus::OK);
    mesh_session::cancel_pairing();
    mesh_session::process(4000 + mesh_pairing::PAIRING_TIMEOUT_MS);
    assert(g_failed_count == 1);
    assert(mesh_session::pairing_state() == mesh_pairing::State::FAILED);
    assert(mesh_session::pairing_fail_reason() == why);
  }
  std::printf("PASS test_a_cancel_on_a_failed_pairing_fires_nothing_again  (timeout, canceled)\n");
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

/* ── F136 — a Canary in an opera leaves it before it joins one ───────────
 *
 * PAIR_JOIN on a Canary that holds an opera is OPERA_EXISTS (409
 * opera_exists from POST /api/mesh/pair/join): no pairing number is taken,
 * nothing is sent, no callback runs, and the opera, its name and its
 * members stay as they were. Before F136 the join ran beside the old opera:
 * the joiner's PairedCallback installed the new secret over the old one and
 * kept every old member, so they sat in the new opera's slots, never heard
 * (their frames fail the opera_id check). After a LEAVE the same request
 * starts a joiner. On the code before F136 the first join answers OK with
 * pairing number 1 and a DISCOVER on the air. */
void test_a_join_waits_for_a_leave() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x6B + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_opera_name("Den");
  const uint8_t mac_m[6] = {0x02, 0x6B, 0x6B, 0x6B, 0x6B, 0x01};
  add_bound_member(mac_m);   /* an old member: registered and bound */
  uint8_t id_before[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(id_before));
  mesh_session::process(10);
  g_outs.clear();
  g_paired_fired = false;
  g_failed_fired = false;

  mesh_session::RequestResult res;
  const mesh_session::Request join = make_request(mesh_session::RequestType::PAIR_JOIN);
  const mesh_session::Request leave = make_request(mesh_session::RequestType::LEAVE);
  assert(mesh_session::submit_request(join));
  mesh_session::process(20);
  assert(mesh_session::take_request_result(&res));
  assert(res.type == mesh_session::RequestType::PAIR_JOIN);
  assert(res.status == mesh_session::RequestStatus::OPERA_EXISTS);
  assert(res.pairing_seq == 0 && mesh_session::pairing_seq() == 0);
  assert(mesh_session::pairing_state() == mesh_pairing::State::IDLE);
  assert(g_outs.empty());                           /* no DISCOVER */
  assert(!g_paired_fired && !g_failed_fired);
  uint8_t id_after[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(id_after));
  assert(std::memcmp(id_before, id_after, sizeof(id_before)) == 0);
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(name, sizeof(name));
  assert(std::strcmp(name, "Den") == 0);
  assert(mesh_session::trusted_peer_count() == 1);
  assert(mesh_transport::has_peer(mac_m));
  assert(has(status_body(), "\"pairing_seq\":0,\"pairing_result\":\"none\""));

  /* Leave, then join: the same request now starts a joiner. */
  assert(mesh_session::submit_request(leave));
  mesh_session::process(30);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && res.notified);
  assert(!mesh_session::has_opera() && mesh_session::trusted_peer_count() == 0);
  g_outs.clear();
  assert(mesh_session::submit_request(join));
  mesh_session::process(40);
  assert(mesh_session::take_request_result(&res));
  assert(res.status == mesh_session::RequestStatus::OK && res.pairing_seq == 1);
  assert(mesh_session::pairing_state() == mesh_pairing::State::DISCOVERING_JOINER);
  static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  assert(g_outs.size() == 1 && std::memcmp(g_outs[0].mac, BCAST, 6) == 0);
  assert(g_outs[0].bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_DISCOVER));
  mesh_session::cancel_pairing();
  std::printf("PASS test_a_join_waits_for_a_leave\n");
}

/* ── F162 — a member announces itself once: after it pairs, after a boot ──
 *
 * One signed HEARTBEAT (registry 16, no payload) to every bound member, on
 * each side right after NOTIFY_PAIRED and once in the first running pass
 * after a boot restore that bound a member. Before F162 a PIO member sent
 * nothing after it paired: the initiator's COMPLETE copies ran their whole
 * 5-minute window, and both sides read CONNECTING with the other OFFLINE
 * ("never") until one of them sent an opera frame. Since F324 the same
 * heartbeat also goes on a timer, every HEARTBEAT_INTERVAL_MS +/-
 * HEARTBEAT_JITTER_MS, each one re-arming it; the tests below admit those
 * and nothing else (assert_only_member_heartbeats), and hold their spacing
 * to the constants (check_beat_spacing). */

/* Two booted sessions in two processes: mesh_session is a singleton, so the
 * joiner runs in a child of this process and the two trade every frame over
 * pipes, in lockstep, one loop pass each per step (main.cpp's order: the
 * transport pass, then process()). The child answers each step with what it
 * sent; a failed assert in it closes the pipe and fails the parent. */
std::string peers_body(uint32_t now_ms);   /* below */

namespace two_canaries {

const uint8_t MAC_I[6] = {0x24, 0x0A, 0xC4, 0x16, 0x20, 0x01};
const uint8_t MAC_J[6] = {0x24, 0x0A, 0xC4, 0x16, 0x20, 0x02};
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum Cmd : uint8_t { STEP = 1, REPORT = 2, QUIT = 3 };

void put(int fd, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  while (n > 0) {
    const ssize_t w = ::write(fd, b, n);
    assert(w > 0);
    b += w;
    n -= (size_t)w;
  }
}
bool get(int fd, void* p, size_t n) {
  uint8_t* b = static_cast<uint8_t*>(p);
  while (n > 0) {
    const ssize_t r = ::read(fd, b, n);
    if (r <= 0) return false;
    b += r;
    n -= (size_t)r;
  }
  return true;
}
void put_frames(int fd, const std::vector<OutFrame>& v) {
  const uint32_t n = (uint32_t)v.size();
  put(fd, &n, sizeof(n));
  for (const auto& f : v) {
    const uint32_t len = (uint32_t)f.bytes.size();
    put(fd, f.mac, 6);
    put(fd, &len, sizeof(len));
    put(fd, f.bytes.data(), len);
  }
}
std::vector<OutFrame> get_frames(int fd) {
  uint32_t n = 0;
  assert(get(fd, &n, sizeof(n)));
  std::vector<OutFrame> v(n);
  for (auto& f : v) {
    uint32_t len = 0;
    assert(get(fd, f.mac, 6) && get(fd, &len, sizeof(len)));
    f.bytes.resize(len);
    assert(get(fd, f.bytes.data(), len));
  }
  return v;
}
void put_str(int fd, const std::string& s) {
  const uint32_t n = (uint32_t)s.size();
  put(fd, &n, sizeof(n));
  put(fd, s.data(), n);
}
std::string get_str(int fd) {
  uint32_t n = 0;
  assert(get(fd, &n, sizeof(n)));
  std::string s(n, '\0');
  assert(get(fd, &s[0], n));
  return s;
}

/* A Canary booting: the transport and the session up, main.cpp's
 * PairedCallback stand-in (set the joiner's secret, register the partner),
 * the opera's secret and name when it holds one. */
void boot(const uint8_t pub[32], const uint8_t priv[32], const uint8_t* secret, uint32_t t) {
  reset_world();
  mesh_session::deinit();
  assert(mesh_session::init(pub, priv));
  mesh_session::set_paired_callback(on_paired_register);
  mesh_session::set_failed_callback(on_failed);
  mesh_session::set_code_ready_callback(on_code_ready);
  assert(mesh_session::start());
  if (secret != nullptr) {
    assert(mesh_session::set_opera_secret(secret));
    mesh_session::set_opera_name("Den");
  }
  mesh_transport::test::set_now_ms(t);
}

/* One loop pass on this side: the frames the other side sent this way
 * arrive from `from`, then the transport pass, then process(), with a REST
 * request in the slot first when `req` is not NONE. Returns what this side
 * sent and the request's status (OK when none was asked). */
std::vector<OutFrame> pass(const uint8_t from[6], const std::vector<OutFrame>& in, uint32_t t,
                           mesh_session::RequestType req, const uint8_t* secret,
                           mesh_session::RequestStatus* status) {
  mesh_transport::test::set_now_ms(t);
  g_outs.clear();
  for (const auto& f : in) mesh_transport::test::inject_recv(from, f.bytes.data(), f.bytes.size(), -40);
  mesh_transport::process();
  if (req != mesh_session::RequestType::NONE) {
    mesh_session::Request r = make_request(req);
    if (secret != nullptr) std::memcpy(r.opera_secret, secret, sizeof(r.opera_secret));
    assert(mesh_session::submit_request(r));
  }
  mesh_session::process(t);
  *status = mesh_session::RequestStatus::OK;
  mesh_session::RequestResult res;
  if (req != mesh_session::RequestType::NONE) {
    assert(mesh_session::take_request_result(&res));
    *status = res.status;
  }
  std::vector<OutFrame> out = g_outs;
  g_outs.clear();
  return out;
}

/* The frames among `sent` addressed to `to` (or broadcast), as they arrive. */
std::vector<OutFrame> towards(const std::vector<OutFrame>& sent, const uint8_t to[6]) {
  std::vector<OutFrame> v;
  for (const auto& f : sent) {
    if (std::memcmp(f.mac, to, 6) == 0 || std::memcmp(f.mac, BCAST, 6) == 0) v.push_back(f);
  }
  return v;
}

size_t count_type(const std::vector<OutFrame>& v, mesh_session::MsgType t) {
  size_t n = 0;
  for (const auto& f : v) n += (!f.bytes.empty() && f.bytes[0] == static_cast<uint8_t>(t)) ? 1 : 0;
  return n;
}
size_t count_heartbeats(const std::vector<OutFrame>& v, const uint8_t to[6]) {
  size_t n = 0;
  for (const auto& f : v) n += (is_heartbeat(f.bytes) && std::memcmp(f.mac, to, 6) == 0) ? 1 : 0;
  return n;
}

/* The joiner's side: steps until told to quit. */
[[noreturn]] void run_joiner(int rd, int wr, const uint8_t j_pub[32], const uint8_t j_priv[32]) {
  boot(j_pub, j_priv, nullptr, 0);
  for (;;) {
    uint8_t cmd = 0;
    if (!get(rd, &cmd, 1)) _exit(3);
    if (cmd == QUIT) _exit(0);
    uint32_t t = 0;
    assert(get(rd, &t, sizeof(t)));
    if (cmd == REPORT) {
      mesh_transport::test::set_now_ms(t);
      put_str(wr, status_body());
      put_str(wr, peers_body(t));
      continue;
    }
    uint8_t req = 0;
    assert(get(rd, &req, 1));
    const std::vector<OutFrame> in = get_frames(rd);
    mesh_session::RequestStatus st;
    const std::vector<OutFrame> out =
        pass(MAC_I, in, t, static_cast<mesh_session::RequestType>(req), nullptr, &st);
    const uint8_t st_b = static_cast<uint8_t>(st);
    put(wr, &st_b, 1);
    put_frames(wr, out);
  }
}

struct Joiner {
  int to_child = -1, from_child = -1;
  pid_t pid = -1;
  std::vector<OutFrame> step(uint32_t t, mesh_session::RequestType req,
                             const std::vector<OutFrame>& in,
                             mesh_session::RequestStatus* st) {
    const uint8_t cmd = STEP, r = static_cast<uint8_t>(req);
    put(to_child, &cmd, 1);
    put(to_child, &t, sizeof(t));
    put(to_child, &r, 1);
    put_frames(to_child, in);
    uint8_t st_b = 0;
    assert(get(from_child, &st_b, 1));   /* EOF: the joiner's assert failed */
    *st = static_cast<mesh_session::RequestStatus>(st_b);
    return get_frames(from_child);
  }
  void report(uint32_t t, std::string* status, std::string* peers) {
    const uint8_t cmd = REPORT;
    put(to_child, &cmd, 1);
    put(to_child, &t, sizeof(t));
    *status = get_str(from_child);
    *peers  = get_str(from_child);
  }
  void quit() {
    const uint8_t cmd = QUIT;
    put(to_child, &cmd, 1);
    int wst = 0;
    assert(waitpid(pid, &wst, 0) == pid);
    assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    close(to_child);
    close(from_child);
  }
};

Joiner fork_joiner(const uint8_t j_pub[32], const uint8_t j_priv[32]) {
  int down[2], up[2];
  assert(pipe(down) == 0 && pipe(up) == 0);
  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(down[1]);
    close(up[0]);
    run_joiner(down[0], up[1], j_pub, j_priv);
  }
  close(down[0]);
  close(up[1]);
  Joiner j;
  j.to_child = down[1];
  j.from_child = up[0];
  j.pid = pid;
  return j;
}

}  /* namespace two_canaries */

/* Two booted Canaries pair through the REST requests, every frame carried
 * between them. Each side's NOTIFY_PAIRED sends the other one heartbeat:
 * the joiner's reaches the initiator in the next pass and ends its COMPLETE
 * copies there, and no copy goes out after it (the first would be due 2 s
 * after the COMPLETE); both read ACTIVE and see the other CONNECTED, with a
 * real age, not OFFLINE "never". Nothing more from either in the next ten
 * seconds. Then ten idle minutes (F324): each side sends only heartbeats,
 * each to the other's address, verified under the sender's key, spaced by
 * the constants from its paired one, and both still read ACTIVE with the
 * other CONNECTED and at most one longest gap old. On the code before F162
 * the joiner sends no heartbeat, the copies run, and both read CONNECTING
 * with the other OFFLINE "never"; before F324 not one frame went out in
 * the ten minutes, and both read CONNECTING with the other OFFLINE, about
 * 610 s old. */
void test_two_paired_canaries_hear_each_other_at_once() {
  using namespace two_canaries;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x62 + i);
  uint8_t i_pub[32], i_priv[32], j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  Joiner joiner = fork_joiner(j_pub, j_priv);
  boot(i_pub, i_priv, S, 0);

  using RT = mesh_session::RequestType;
  mesh_session::RequestStatus st_i, st_j;
  std::vector<OutFrame> to_i, to_j, sent_i, sent_j;
  /* One step: the initiator's pass, then the joiner's, each taking what
   * the other sent in its last pass. */
  auto step = [&](uint32_t t, RT req_i, RT req_j) {
    sent_i = pass(MAC_J, to_i, t, req_i, req_i == RT::PAIR_START ? S : nullptr, &st_i);
    to_j = towards(sent_i, MAC_J);
    sent_j = joiner.step(t, req_j, to_j, &st_j);
    to_i = towards(sent_j, MAC_I);
  };

  step(1000, RT::PAIR_START, RT::PAIR_JOIN);   /* the DISCOVER */
  assert(st_i == mesh_session::RequestStatus::OK && st_j == mesh_session::RequestStatus::OK);
  step(1100, RT::NONE, RT::NONE);              /* OFFER, ACCEPT: the codes */
  step(1200, RT::NONE, RT::NONE);
  assert(mesh_session::pairing_state() == mesh_pairing::State::AWAITING_CONFIRM);
  step(1300, RT::PAIR_CONFIRM, RT::PAIR_CONFIRM);   /* both owners confirm */
  assert(st_i == mesh_session::RequestStatus::OK && st_j == mesh_session::RequestStatus::OK);

  /* The initiator takes the joiner's CONFIRM: CONFIRM + COMPLETE, then its
   * NOTIFY_PAIRED's heartbeat. The joiner takes the COMPLETE, pairs, sends
   * its own heartbeat, and takes the initiator's. */
  step(1400, RT::NONE, RT::NONE);
  assert(g_paired_fired && mesh_session::trusted_peer_count() == 1);
  assert(count_type(sent_i, mesh_session::MsgType::PAIR_COMPLETE) == 1);
  assert(count_heartbeats(sent_i, MAC_J) == 1 && sent_i.size() == 3);
  assert(is_heartbeat(sent_i.back().bytes));            /* after the COMPLETE */
  assert(count_heartbeats(sent_j, MAC_I) == 1 && sent_j.size() == 1);
  /* Each paired heartbeat armed its side's timer (F324): the spacing below
   * counts from these. */
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t to_j_mac[1][6] = {{MAC_J[0], MAC_J[1], MAC_J[2], MAC_J[3], MAC_J[4], MAC_J[5]}};
  const uint8_t to_i_mac[1][6] = {{MAC_I[0], MAC_I[1], MAC_I[2], MAC_I[3], MAC_I[4], MAC_I[5]}};
  const uint32_t paired_i =
      assert_only_member_heartbeats({sent_i.back()}, i_pub, opera_id, to_j_mac, 1)[0].timestamp;
  const uint32_t paired_j =
      assert_only_member_heartbeats(sent_j, j_pub, opera_id, to_i_mac, 1)[0].timestamp;

  /* The next pass takes it; from then on neither side sends anything: no
   * copy at 2 s, none for the rest of the window. */
  size_t later_i = 0, later_j = 0, copies = 0;
  for (uint32_t t = 1500; t <= 1400 + 10000; t += 100) {
    step(t, RT::NONE, RT::NONE);
    later_i += sent_i.size();
    later_j += sent_j.size();
    copies += count_type(sent_i, mesh_session::MsgType::PAIR_COMPLETE);
  }
  assert(copies == 0 && later_i == 0 && later_j == 0);

  std::string body_j, peers_j;
  joiner.report(11500, &body_j, &peers_j);
  mesh_transport::test::set_now_ms(11500);
  const std::string body_i = status_body();
  const std::string peers_i = peers_body(11500);
  for (const std::string* b : {&body_i, static_cast<const std::string*>(&body_j)}) {
    assert(has(*b, "\"state\":\"ACTIVE\""));
    assert(has(*b, "\"peers_total\":1,\"peers_online\":1"));
  }
  for (const std::string* p : {&peers_i, static_cast<const std::string*>(&peers_j)}) {
    assert(has(*p, "\"state\":\"CONNECTED\",\"last_seen_sec\":10,"));
    assert(!has(*p, "OFFLINE"));
  }

  /* Ten idle minutes, a pass a second: each side's heartbeats and nothing
   * else (F324), each to the other, spaced from its paired one. */
  const uint32_t W0 = 12000, W1 = 12000 + 600000, pass_ms = 1000;
  std::vector<OutFrame> idle_i, idle_j;
  for (uint32_t t = W0; t <= W1; t += pass_ms) {
    step(t, RT::NONE, RT::NONE);
    idle_i.insert(idle_i.end(), sent_i.begin(), sent_i.end());
    idle_j.insert(idle_j.end(), sent_j.begin(), sent_j.end());
  }
  const std::vector<Beat> beats_i = assert_only_member_heartbeats(idle_i, i_pub, opera_id, to_j_mac, 1);
  const std::vector<Beat> beats_j = assert_only_member_heartbeats(idle_j, j_pub, opera_id, to_i_mac, 1);
  size_t lo_i = 0, hi_i = 0, lo_j = 0, hi_j = 0;
  check_beat_spacing(beat_times_to(beats_i, MAC_J), paired_i, W0, W1, pass_ms, &lo_i, &hi_i);
  check_beat_spacing(beat_times_to(beats_j, MAC_I), paired_j, W0, W1, pass_ms, &lo_j, &hi_j);

  /* Both still read the opera ACTIVE and the other CONNECTED, at most one
   * longest gap old (a beat sent in the last pass lands in the next). */
  joiner.report(W1, &body_j, &peers_j);
  mesh_transport::test::set_now_ms(W1);
  const std::string end_body_i = status_body();
  const std::string end_peers_i = peers_body(W1);
  const unsigned max_age_sec = beat_gap_max(pass_ms) / 1000;
  for (const std::string* b : {&end_body_i, static_cast<const std::string*>(&body_j)}) {
    assert(has(*b, "\"state\":\"ACTIVE\""));
    assert(has(*b, "\"peers_total\":1,\"peers_online\":1"));
  }
  for (const std::string* p : {&end_peers_i, static_cast<const std::string*>(&peers_j)}) {
    assert(has(*p, "\"state\":\"CONNECTED\",\"last_seen_sec\":"));
    assert(!has(*p, "OFFLINE") && !has(*p, "STALE"));
    assert(last_seen_sec_of(*p) <= max_age_sec);
  }
  joiner.quit();
  std::printf("PASS test_two_paired_canaries_hear_each_other_at_once"
              "  (idle 10 min: %zu heartbeats, %zu-%zu allowed; %zu, %zu-%zu)\n",
              beats_i.size(), lo_i, hi_i, beats_j.size(), lo_j, hi_j);
}

/* A reboot: main.cpp registers the stored members and restore_peer_macs()
 * binds their stored addresses, as setup() does. The first pass sends
 * exactly one heartbeat to each bound member, signed by this device under
 * the opera with a counter past the restored one, and to nobody else (not
 * a pairing partner whose address the transport holds: F101); a member the
 * restore could not bind gets none. Then, over an hour of passes, only the
 * periodic heartbeats (F324): to the two bound members, never to the
 * partner or the unbound two, each member's rounds spaced from the boot one
 * by the constants. Nothing at all, and no counter spent, from a boot whose
 * restore bound no member, nor from one whose bound member is gone before
 * the first pass. A mesh off at boot sends nothing while off, the boot
 * heartbeat on its first pass after it is turned on, then the periodic
 * ones from there. On the code before F162 the first pass sends nothing;
 * before F324 nothing went out over the hour. */
void test_a_reboot_announces_itself_once_to_each_member() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x91 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::restore_outbound_counter(4000);
  const uint8_t macs[3][6] = {{0x24, 0x0A, 0xC4, 0x16, 0x30, 0x01},
                              {0x24, 0x0A, 0xC4, 0x16, 0x30, 0x02},
                              {0x24, 0x0A, 0xC4, 0x16, 0x30, 0x03}};
  uint8_t fps[4][8];
  uint8_t stored_macs[4][6];
  for (int m = 0; m < 4; ++m) {
    uint8_t mp[32], mk[32];
    assert(mesh_crypto::ed25519_generate_keypair(mp, mk));
    assert(mesh_session::register_trusted_peer(mp));
    mesh_crypto::compute_fingerprint(mp, fps[m]);
    std::memcpy(stored_macs[m], macs[m < 3 ? m : 2], 6);   /* the 4th shares the 3rd's */
  }
  mesh_session::StoredMacVerdict verdicts[4];
  /* Members 0 and 1 bound; 2 and 3 share an address, so neither is (F119). */
  assert(mesh_session::restore_peer_macs(fps, stored_macs, 4, verdicts) == 2);

  /* A pairing partner's address in the transport table before the first
   * pass: an outsider answering a pairing this device started. */
  const uint8_t mac_o[6] = {0x24, 0x0A, 0xC4, 0x16, 0x30, 0x0F};
  outsider_answers_the_pairing(S, mac_o);
  assert(mesh_transport::has_peer(mac_o));
  g_outs.clear();

  mesh_session::process(5000);
  std::vector<OutFrame> beats;
  for (const auto& o : g_outs) {
    if (is_heartbeat(o.bytes)) beats.push_back(o);
  }
  assert(beats.size() == 2);   /* none to the partner, none to the unbound */
  for (int m = 0; m < 2; ++m) {
    assert(std::memcmp(beats[m].mac, macs[m], 6) == 0);
    mesh_envelope::Header h;
    const uint8_t* payload = nullptr;
    size_t payload_len = 99;
    assert(mesh_envelope::parse_and_verify(beats[m].bytes.data(), beats[m].bytes.size(), pub,
                                           &h, &payload, &payload_len));
    assert(payload_len == 0 && h.counter > 4000);
    uint8_t id[mesh_crypto::OPERA_ID_LEN];
    assert(mesh_session::get_opera_id(id) && std::memcmp(h.opera_id, id, sizeof(id)) == 0);
  }
  mesh_session::cancel_pairing();

  /* An hour of passes, the transport's clock moving with them as main.cpp's
   * millis() does: the members' rounds and nothing else. */
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  assert(mesh_session::get_opera_id(opera_id));
  const uint32_t W0 = 6000, W1 = 6000 + 3600000, pass_ms = 1000;
  g_outs.clear();
  for (uint32_t t = W0; t <= W1; t += pass_ms) {
    mesh_transport::test::set_now_ms(t);
    mesh_session::process(t);
  }
  const std::vector<Beat> boot = assert_only_member_heartbeats(beats, pub, opera_id, macs, 2);
  const std::vector<Beat> hour = assert_only_member_heartbeats(g_outs, pub, opera_id, macs, 2);
  assert(sent_to(mac_o) == 0 && sent_to(macs[2]) == 0);
  size_t lo = 0, hi = 0;
  for (int m = 0; m < 2; ++m) {
    assert(boot[m].timestamp == 5000);
    check_beat_spacing(beat_times_to(hour, macs[m]), boot[m].timestamp, W0, W1, pass_ms, &lo, &hi);
  }
  const size_t rounds = beat_times_to(hour, macs[0]).size();
  assert(beat_times_to(hour, macs[1]).size() == rounds);   /* one frame per round, to each */

  /* A boot whose restore bound nobody owes nobody. */
  stand_up_session(S, pub, priv);
  uint8_t lone_pub[32], lone_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(lone_pub, lone_priv));
  assert(mesh_session::register_trusted_peer(lone_pub));
  const uint8_t one_mac[1][6] = {{0x24, 0x0A, 0xC4, 0x16, 0x30, 0x0A}};
  const uint8_t one_fp[1][8] = {{0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}};   /* not a member (F120) */
  mesh_session::StoredMacVerdict v1[1];
  assert(mesh_session::restore_peer_macs(one_fp, one_mac, 1, v1) == 0);
  const uint64_t ctr_none = mesh_session::outbound_counter();
  g_outs.clear();
  for (uint32_t t = 1000; t <= 61000; t += 1000) mesh_session::process(t);
  assert(g_outs.empty() && mesh_session::outbound_counter() == ctr_none);

  /* F162's review: a boot whose one bound member is gone before the first
   * pass (main.cpp does nothing between the restore and the loop; this is
   * send_heartbeat's own guard): nothing is signed and no counter spent. */
  stand_up_session(S, pub, priv);
  uint8_t gone_pub[32], gone_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(gone_pub, gone_priv));
  assert(mesh_session::register_trusted_peer(gone_pub));
  uint8_t gone_fp[1][8];
  mesh_crypto::compute_fingerprint(gone_pub, gone_fp[0]);
  const uint8_t gone_mac[1][6] = {{0x24, 0x0A, 0xC4, 0x16, 0x30, 0x0C}};
  mesh_session::StoredMacVerdict v3[1];
  assert(mesh_session::restore_peer_macs(gone_fp, gone_mac, 1, v3) == 1);
  assert(mesh_session::unregister_trusted_peer(gone_fp[0]));
  const uint64_t ctr_gone = mesh_session::outbound_counter();
  g_outs.clear();
  mesh_session::process(1000);
  assert(g_outs.empty() && mesh_session::outbound_counter() == ctr_gone);

  /* A boot with the mesh turned off (POST /api/mesh/enable, main.cpp turns
   * it off before the restore): nothing while it is off, the one heartbeat
   * at the first pass after it is turned on, then the periodic ones. */
  stand_up_session(S, pub, priv);
  mesh_session::set_enabled(false);
  uint8_t off_pub[32], off_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(off_pub, off_priv));
  assert(mesh_session::register_trusted_peer(off_pub));
  uint8_t off_fp[1][8];
  mesh_crypto::compute_fingerprint(off_pub, off_fp[0]);
  const uint8_t off_mac[1][6] = {{0x24, 0x0A, 0xC4, 0x16, 0x30, 0x0B}};
  mesh_session::StoredMacVerdict v2[1];
  assert(mesh_session::restore_peer_macs(off_fp, off_mac, 1, v2) == 1);
  const uint64_t ctr_off = mesh_session::outbound_counter();
  g_outs.clear();
  for (uint32_t t = 1000; t <= 61000; t += 1000) mesh_session::process(t);
  assert(g_outs.empty() && mesh_session::outbound_counter() == ctr_off);
  mesh_session::set_enabled(true);
  mesh_session::process(62000);
  assert(g_outs.size() == 1 && is_heartbeat(g_outs[0].bytes));
  assert(std::memcmp(g_outs[0].mac, off_mac[0], 6) == 0);
  const uint32_t boot_beat =
      assert_only_member_heartbeats(g_outs, pub, opera_id, off_mac, 1)[0].timestamp;
  assert(boot_beat == 62000);
  g_outs.clear();
  for (uint32_t t = 63000; t <= 123000; t += 1000) mesh_session::process(t);
  const std::vector<Beat> after_on = assert_only_member_heartbeats(g_outs, pub, opera_id, off_mac, 1);
  size_t lo_on = 0, hi_on = 0;
  check_beat_spacing(beat_times_to(after_on, off_mac[0]), boot_beat, 63000, 123000, 1000, &lo_on,
                     &hi_on);
  std::printf("PASS test_a_reboot_announces_itself_once_to_each_member"
              "  (an hour: %zu rounds, %zu-%zu allowed)\n", rounds, lo, hi);
}

void test_rest_pairing_requests() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xE1 + i);
  uint8_t pub[32], priv[32];
  /* No opera until the rotation below: since F136 a join on a Canary that
   * holds one is OPERA_EXISTS (test_a_join_waits_for_a_leave), and the
   * joins here are about the slot. A start needs none: it carries S. */
  stand_up_session(nullptr, pub, priv);
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

  /* While a rotation runs: start / join say REKEY_IN_FLIGHT (a rotation
   * needs the opera; the join is asked about the rotation first). */
  assert(mesh_session::set_opera_secret(S));
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

/* ── F161 — the status routes read a view the main loop publishes ───────
 *
 * GET /api/mesh and /api/mesh/peers run on the httpd task. Until F161 they
 * read the session's state in place while process() wrote it on the main
 * loop; now they copy the view the main loop last published
 * (mesh_session::read_status), status_body() and peers_body() building
 * each body the way the two handlers do. live_status_body() and
 * live_peers_body() are the pre-F161 handlers' bodies, from the live state:
 * what a read must equal right after a pass, and must not show between
 * passes. */

/* GET /api/mesh/peers as handle_mesh_peers builds it since F199: the rows
 * are the members of the published view at `now_ms`; no store is read. */
std::string peers_body(uint32_t now_ms) {
  mesh_session::StatusView view;
  mesh_session::read_status(&view);
  mesh_api::PeerView rows[mesh_session::MAX_TRUSTED_PEERS];
  const size_t count =
      mesh_api::peer_views_from_status(view, now_ms, rows, mesh_session::MAX_TRUSTED_PEERS);
  char body[mesh_api::PEERS_JSON_CAP];
  assert(mesh_api::build_mesh_peers_json(body, sizeof(body), rows, count));
  return body;
}

/* GET /api/mesh as handle_mesh_status built it before F161: every field
 * read live. */
std::string live_status_body() {
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  const bool have_id = mesh_session::get_opera_id(opera_id);
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(name, sizeof(name));
  mesh_api::PairingReport last;
  last.seq         = mesh_session::pairing_seq();
  last.outcome     = mesh_session::pairing_outcome();
  last.fail_reason = mesh_session::pairing_fail_reason();
  char body[mesh_api::STATUS_JSON_CAP];
  assert(mesh_api::build_mesh_status_json(
      body, sizeof(body), mesh_session::is_enabled(), mesh_session::has_opera(),
      have_id ? opera_id : nullptr, name, mesh_session::pairing_state(),
      mesh_session::trusted_peer_count(), mesh_session::online_peer_count(),
      mesh_session::alerts_received(), mesh_session::pairing_confirmation_code(), &last));
  return body;
}

/* GET /api/mesh/peers as handle_mesh_peers built it before F161: the
 * session's peer links and the transport table read live, joined in the
 * handler. Since F324 the view's state, age and RSSI are the member's last
 * verified frame's, not the transport entry's, so the two agree only where
 * every frame from a member's address verified, as in the F161 tests that
 * compare them; test_liveness_counts_only_verified_frames is where they
 * part. */
std::string live_peers_body(const uint8_t* pubkeys, size_t count, uint32_t now_ms) {
  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  const size_t n_links = mesh_session::get_peer_links(links, mesh_session::MAX_TRUSTED_PEERS);
  mesh_transport::Peer live[16];
  const size_t n_live = mesh_transport::list_peers(live, 16);
  mesh_api::PeerView views[mesh_session::MAX_TRUSTED_PEERS];
  for (size_t i = 0; i < count; ++i) {
    uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
    mesh_crypto::compute_fingerprint(pubkeys + i * mesh_crypto::PUBKEY_LEN, fp);
    static const char kHex[] = "0123456789abcdef";
    for (size_t b = 0; b < mesh_crypto::FINGERPRINT_LEN; ++b) {
      views[i].fingerprint[2 * b]     = kHex[(fp[b] >> 4) & 0xF];
      views[i].fingerprint[2 * b + 1] = kHex[fp[b] & 0xF];
    }
    views[i].fingerprint[mesh_crypto::FINGERPRINT_LEN * 2] = '\0';
    views[i].name[0] = '\0';
    views[i].state = "OFFLINE";
    views[i].last_seen_sec = 0xFFFFFFFFu;
    views[i].rssi = 0;
    views[i].alerts_received = 0;
    for (size_t l = 0; l < n_links; ++l) {
      if (std::memcmp(links[l].fp, fp, mesh_crypto::FINGERPRINT_LEN) != 0) continue;
      views[i].alerts_received = links[l].alerts_received;
      if (!links[l].mac_known) break;
      for (size_t t = 0; t < n_live; ++t) {
        if (!live[t].in_use || std::memcmp(live[t].mac, links[l].mac, 6) != 0) continue;
        switch (live[t].state) {
          case mesh_transport::PeerState::ACTIVE: views[i].state = "CONNECTED"; break;
          case mesh_transport::PeerState::STALE:  views[i].state = "STALE";     break;
          default:                                views[i].state = "OFFLINE";   break;
        }
        views[i].last_seen_sec = (now_ms - live[t].last_seen_ms) / 1000u;
        views[i].rssi = live[t].rssi_dbm;
        break;
      }
      break;
    }
  }
  char body[mesh_api::PEERS_JSON_CAP];
  assert(mesh_api::build_mesh_peers_json(body, sizeof(body), views, count));
  return body;
}

std::string fp_hex_of(const uint8_t pub[mesh_crypto::PUBKEY_LEN]) {
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(pub, fp);
  char hex[mesh_crypto::FINGERPRINT_LEN * 2 + 1];
  for (size_t b = 0; b < mesh_crypto::FINGERPRINT_LEN; ++b) {
    std::snprintf(hex + 2 * b, 3, "%02x", fp[b]);
  }
  return hex;
}

/* The row of `pub`'s fingerprint in the peers body at `now_ms`, or "". */
std::string peer_row(uint32_t now_ms, const uint8_t pub[mesh_crypto::PUBKEY_LEN]) {
  const std::string body = peers_body(now_ms);
  const std::string key = "{\"fingerprint\":\"" + fp_hex_of(pub) + "\"";
  const size_t at = body.find(key);
  if (at == std::string::npos) return "";
  return body.substr(at, body.find('}', at) - at + 1);
}

/* A trusted member with its keys kept, bound to `mac`. */
struct TestMember {
  uint8_t pub[mesh_crypto::PUBKEY_LEN];
  uint8_t priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  uint8_t mac[6];
  uint64_t counter;
};
void make_member(TestMember& m, const uint8_t mac[6], bool bind = true) {
  assert(mesh_crypto::ed25519_generate_keypair(m.pub, m.priv));
  mesh_crypto::compute_fingerprint(m.pub, m.fp);
  std::memcpy(m.mac, mac, 6);
  m.counter = 0;
  assert(mesh_session::register_trusted_peer(m.pub));
  if (bind) assert(mesh_session::bind_peer_mac(m.fp, mac));
}
/* The member sends a verified TAMPER_ALERT from its binding, in a transport
 * pass at transport time `t`. */
void member_speaks(TestMember& m, const uint8_t S[32], uint32_t t, int rssi) {
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_alert_frame(m.pub, m.priv, S, ++m.counter,
                                     mesh_alert::Kind::CAMERA_TAMPER, 3, 7, frame, sizeof(frame));
  assert(n > 0);
  mesh_transport::test::set_now_ms(t);
  mesh_transport::test::inject_recv(m.mac, frame, n, (int8_t)rssi);
  mesh_transport::process();
}

/* Runs first in main(), before any test has published a view: a read is
 * what the session reads before init() — the HTTP server is up before the
 * mesh starts — and the body is the one the live state gave then, byte for
 * byte. */
void test_a_read_before_any_publish_is_the_state_before_init() {
  mesh_session::StatusView v;
  std::memset(&v, 0xA5, sizeof(v));
  mesh_session::read_status(&v);
  assert(v.enabled && !v.has_opera);
  for (size_t i = 0; i < sizeof(v.opera_id); ++i) assert(v.opera_id[i] == 0);
  assert(v.opera_name[0] == '\0');
  assert(v.pairing_state == mesh_pairing::State::IDLE && v.pairing_code == 0);
  assert(v.pairing_seq == 0 && v.pairing_outcome == mesh_pairing::Outcome::NONE &&
         v.pairing_fail_reason == mesh_pairing::FailReason::NONE);
  assert(v.peers_total == 0 && v.peers_online == 0 && v.alerts_received == 0 &&
         v.member_count == 0);
  assert(status_body() == live_status_body());
  assert(has(status_body(), "\"state\":\"NO_OPERA\""));
  std::printf("PASS test_a_read_before_any_publish_is_the_state_before_init\n");
}

/* Between two passes the main loop renames the opera, starts a pairing and
 * adds a member. A read in between is the last pass, whole; the old
 * handler's live read already showed the new name, the pairing and two
 * members. After the next pass the read is the new state, equal to the live
 * body. */
void test_a_status_read_is_the_last_published_pass() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x16 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_opera_name("Home");
  const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x16, 0x00, 0x0A};
  add_bound_member(mac_a);
  mesh_session::process(1000);
  const std::string before = status_body();
  assert(before == live_status_body());
  assert(has(before, "\"opera_name\":\"Home\"") && has(before, "\"peers_total\":1") &&
         has(before, "\"state\":\"CONNECTING\"") &&
         has(before, "\"pairing_seq\":0,\"pairing_result\":\"none\""));

  mesh_session::set_opera_name("Renamed");
  assert(mesh_session::start_pairing_joiner(1001));
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x16, 0x00, 0x0B};
  add_bound_member(mac_b);
  assert(status_body() == before);
  assert(live_status_body() != before);

  mesh_session::process(1002);
  const std::string after = status_body();
  assert(after == live_status_body());
  assert(has(after, "\"opera_name\":\"Renamed\"") && has(after, "\"peers_total\":2") &&
         has(after, "\"state\":\"PAIRING_JOIN\"") &&
         has(after, "\"pairing_seq\":1,\"pairing_result\":\"running\""));
  std::printf("PASS test_a_status_read_is_the_last_published_pass\n");
}

/* The handler collects a request's result as soon as the drain posts it
 * (DONE), which is before the rest of the pass runs; its page then reads
 * GET /api/mesh. The FailedCallback below plays that handler: the pass's
 * pairing tick times a pairing out after the drain ran the rename, so the
 * callback runs between the two, takes the result and reads the status.
 * It must already show the new name: the drain publishes before it posts.
 * Without that publish it read the pass before (the old name). */
bool        g_f161_took = false;
std::string g_f161_read;
void f161_read_like_the_handler(mesh_pairing::FailReason, const uint8_t*) {
  mesh_session::RequestResult res;
  g_f161_took = mesh_session::take_request_result(&res) &&
                res.type == mesh_session::RequestType::SET_NAME &&
                res.status == mesh_session::RequestStatus::OK;
  g_f161_read = status_body();
}

void test_a_read_right_after_a_post_shows_what_it_did() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x17 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_opera_name("Before");
  assert(mesh_session::start_pairing_joiner(100));
  mesh_session::process(200);
  assert(has(status_body(), "\"opera_name\":\"Before\""));
  g_f161_took = false;
  g_f161_read.clear();
  mesh_session::set_failed_callback(f161_read_like_the_handler);
  mesh_session::Request r = make_request(mesh_session::RequestType::SET_NAME);
  std::strcpy(r.name, "After");
  assert(mesh_session::submit_request(r));
  mesh_session::process(100 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(g_f161_took);
  assert(has(g_f161_read, "\"opera_name\":\"After\""));
  /* The pass then ends with the timeout in the view too. */
  const std::string end = status_body();
  assert(has(end, "\"opera_name\":\"After\"") &&
         has(end, "\"pairing_seq\":1,\"pairing_result\":\"failed\",\"pairing_fail_reason\":\"timeout\""));
  assert(end == live_status_body());
  std::printf("PASS test_a_read_right_after_a_post_shows_what_it_did\n");
}

/* A disabled session still publishes every pass: process() returns early
 * while stopped, but the radio keeps running and the transport table keeps
 * ageing, so a member heard before the switch went off reads STALE once
 * its entry is (the live read showed it; a view published only by running
 * passes still said CONNECTED). The switch itself shows at once: the drain
 * published it. */
void test_a_disabled_session_still_publishes_each_pass() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x18 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  TestMember a;
  const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x18, 0x00, 0x0A};
  make_member(a, mac_a);
  member_speaks(a, S, 1000, -48);
  mesh_session::process(1000);
  assert(has(peers_body(2000), "\"state\":\"CONNECTED\",\"last_seen_sec\":1,\"rssi\":-48"));
  assert(has(status_body(), "\"state\":\"ACTIVE\""));

  mesh_session::Request off = make_request(mesh_session::RequestType::SET_ENABLED);
  off.enabled = false;
  assert(mesh_session::submit_request(off));
  mesh_session::process(1100);
  mesh_session::RequestResult res;
  assert(mesh_session::take_request_result(&res) && !res.enabled);
  assert(has(status_body(), "\"state\":\"DISABLED\"") && has(status_body(), "\"enabled\":false"));

  const uint32_t later = 1000 + mesh_transport::PEER_STALE_AFTER_MS + 5000;
  mesh_transport::test::set_now_ms(later);
  mesh_transport::process();
  mesh_session::process(later);
  const std::string rows = peers_body(later);
  assert(has(rows, "\"state\":\"STALE\""));
  assert(rows == live_peers_body(a.pub, 1, later));
  assert(status_body() == live_status_body());
  std::printf("PASS test_a_disabled_session_still_publishes_each_pass\n");
}

/* deinit() wipes the session and publishes what it left: no opera, no
 * members, no pairing — not the opera it had. */
void test_deinit_publishes_the_wiped_session() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x19 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  mesh_session::set_opera_name("Home");
  const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x19, 0x00, 0x0A};
  add_bound_member(mac_a);
  assert(mesh_session::start_pairing_joiner(10));
  mesh_session::process(20);
  assert(has(status_body(), "\"opera_name\":\"Home\""));
  mesh_session::deinit();
  const std::string b = status_body();
  assert(b == live_status_body());
  assert(has(b, "\"state\":\"NO_OPERA\",\"opera_id\":\"\",\"opera_name\":\"\"") &&
         has(b, "\"peers_total\":0") && has(b, "\"pairing_seq\":0,\"pairing_result\":\"none\""));
  /* deinit() clears the name by its first byte; the view copies no
   * fragment of the old one from behind it. */
  mesh_session::StatusView v;
  std::memset(&v, 0xA5, sizeof(v));
  mesh_session::read_status(&v);
  for (size_t i = 0; i < sizeof(v.opera_name); ++i) assert(v.opera_name[i] == '\0');
  std::printf("PASS test_deinit_publishes_the_wiped_session\n");
}

/* The view keeps the pairing code only while GET /api/mesh shows it. A
 * joiner that paired still holds its code in the pairing context (live
 * pairing_confirmation_code() is not 0), and the body never showed it in
 * that state; the view holds 0 there, so no second copy outlives the
 * code's screen. */
void test_the_view_holds_the_code_only_while_it_is_shown() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x1A + i);
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x1A, 0x00, 0x01};
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x1A, 0x00, 0x11};
  mesh_session::set_paired_callback(on_paired_register);
  assert(mesh_session::start_pairing_joiner(1000));
  mesh_pairing::PairingContext ci;
  mesh_pairing::context_init(ci);
  uint8_t i_pub[32], i_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  mesh_pairing::Action a = mesh_pairing::start_initiator(ci, i_pub, i_priv, S, "Home", 1000);
  feed_pure(ci, me, last_to((const uint8_t[6]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), 1010, &a);
  const std::vector<uint8_t> offer = wire(a);
  mesh_transport::test::inject_recv(mac_i, offer.data(), offer.size(), -40);
  mesh_transport::process();
  mesh_session::process(1015);
  mesh_session::StatusView v;
  mesh_session::read_status(&v);
  assert(v.pairing_state == mesh_pairing::State::AWAITING_CONFIRM);
  assert(v.pairing_code == mesh_session::pairing_confirmation_code());
  char code_field[40];
  std::snprintf(code_field, sizeof(code_field), "\"pairing_code\":%u", (unsigned)v.pairing_code);
  assert(has(status_body(), code_field));

  feed_pure(ci, me, last_to(mac_i), 1020, &a);
  assert(mesh_session::confirm_pairing_code(1030));
  feed_pure(ci, me, last_to(mac_i), 1040, &a);
  a = mesh_pairing::confirm_code(ci, 1050);
  inject_all(mac_i, wire_all(a));
  mesh_session::process(1060);
  assert(mesh_session::pairing_state() == mesh_pairing::State::PAIRED);
  assert(mesh_session::pairing_confirmation_code() != 0);
  mesh_session::read_status(&v);
  assert(v.pairing_code == 0);
  assert(!has(status_body(), "pairing_code") && status_body() == live_status_body());
  std::printf("PASS test_the_view_holds_the_code_only_while_it_is_shown\n");
}

/* GET /api/mesh/peers: one row per member of the published view, in the
 * session's table order (since F199; it was one per persisted pubkey, in
 * NVS order, so a stored pubkey the session does not trust had a row, as
 * this test pinned, and now has none). A member heard a second ago reads
 * CONNECTED with its RSSI; one heard 97 s ago STALE; one bound but never
 * heard OFFLINE, "never"; one heard whose address left the transport table
 * OFFLINE, "never", with its alert still counted. Each row equals the
 * pre-F161 handler's join of the live state after the same pass (every
 * field kept), and the age is counted at the read. */
void test_the_peer_list_joins_each_member_from_the_view() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x1B + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  TestMember a, b, c, d, e;
  const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x1B, 0x00, 0x0A};
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x1B, 0x00, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x1B, 0x00, 0x0C};
  const uint8_t mac_d[6] = {0x24, 0x0A, 0xC4, 0x1B, 0x00, 0x0D};
  make_member(a, mac_a);
  make_member(b, mac_b);
  make_member(c, mac_c);
  make_member(d, mac_d);
  assert(mesh_crypto::ed25519_generate_keypair(e.pub, e.priv));   /* stored, not trusted */
  (void)e;
  member_speaks(b, S, 1000, -67);
  member_speaks(d, S, 1000, -55);
  mesh_transport::remove_peer(mac_d);
  member_speaks(a, S, 95000, -41);
  member_speaks(a, S, 95500, -42);
  mesh_transport::test::set_now_ms(96000);
  mesh_transport::process();
  mesh_session::process(96000);

  uint8_t keys[4 * mesh_crypto::PUBKEY_LEN];
  const TestMember* order[4] = {&a, &b, &c, &d};   /* the table order */
  for (size_t i = 0; i < 4; ++i) std::memcpy(keys + i * 32, order[i]->pub, 32);
  const std::string rows = peers_body(98500);
  const std::string want =
      "{\"ok\":true,\"peers\":["
      "{\"fingerprint\":\"" + fp_hex_of(a.pub) + "\",\"name\":\"\",\"state\":\"CONNECTED\","
      "\"last_seen_sec\":3,\"rssi\":-42,\"alerts_received\":2},"
      "{\"fingerprint\":\"" + fp_hex_of(b.pub) + "\",\"name\":\"\",\"state\":\"STALE\","
      "\"last_seen_sec\":97,\"rssi\":-67,\"alerts_received\":1},"
      "{\"fingerprint\":\"" + fp_hex_of(c.pub) + "\",\"name\":\"\",\"state\":\"OFFLINE\","
      "\"last_seen_sec\":4294967295,\"rssi\":0,\"alerts_received\":0},"
      "{\"fingerprint\":\"" + fp_hex_of(d.pub) + "\",\"name\":\"\",\"state\":\"OFFLINE\","
      "\"last_seen_sec\":4294967295,\"rssi\":0,\"alerts_received\":1}"
      "]}";
  assert(rows == want);
  assert(!has(rows, fp_hex_of(e.pub).c_str()));
  assert(rows == live_peers_body(keys, 4, 98500));
  /* Counted at the read: the same view a minute later is a minute older. */
  assert(has(peer_row(158500, a.pub), "\"last_seen_sec\":63,"));
  assert(has(status_body(), "\"peers_total\":4,\"peers_online\":1,\"alerts_received\":4"));
  assert(status_body() == live_status_body());

  /* Between passes the view stands: B speaks again in a transport pass, and
   * until the session pass the row still shows its old sighting. */
  member_speaks(b, S, 97000, -60);
  assert(has(peer_row(98500, b.pub), "\"state\":\"STALE\",\"last_seen_sec\":97,\"rssi\":-67"));
  mesh_session::process(97000);
  assert(has(peer_row(98500, b.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":1,\"rssi\":-60"));

  /* Unheard past PEER_OFFLINE_AFTER_MS, a member's transport entry turns
   * OFFLINE but stays in the table: the row is still live, so it reads
   * OFFLINE with its real age and RSSI, not "never" and 0, and never
   * CONNECTED (review: no case reached this row). */
  const uint32_t gone = 97000 + mesh_transport::PEER_OFFLINE_AFTER_MS + 5000;
  mesh_transport::test::set_now_ms(gone);
  mesh_transport::process();
  mesh_session::process(gone);
  {
    mesh_session::StatusView aged;
    mesh_session::read_status(&aged);
    bool seen_b = false;
    for (size_t m = 0; m < aged.member_count; ++m) {
      if (std::memcmp(aged.members[m].fp, b.fp, mesh_crypto::FINGERPRINT_LEN) != 0) continue;
      seen_b = aged.members[m].live &&
               aged.members[m].link_state == mesh_transport::PeerState::OFFLINE;
    }
    assert(seen_b);
  }
  const std::string aged_rows = peers_body(gone);
  assert(aged_rows == live_peers_body(keys, 4, gone));
  assert(has(peer_row(gone, b.pub), "\"state\":\"OFFLINE\",\"last_seen_sec\":305,\"rssi\":-60,"));
  assert(has(peer_row(gone, a.pub), "\"state\":\"OFFLINE\",\"last_seen_sec\":306,\"rssi\":-42,"));
  assert(!has(aged_rows, "CONNECTED") && !has(aged_rows, "STALE"));
  assert(has(status_body(), "\"peers_total\":4,\"peers_online\":0,"));
  assert(status_body() == live_status_body());

  /* Null rows answer nothing; a short array takes the first rows. */
  mesh_session::StatusView v;
  mesh_session::read_status(&v);
  mesh_api::PeerView one[1];
  assert(mesh_api::peer_views_from_status(v, 0, nullptr, 1) == 0);
  assert(mesh_api::peer_views_from_status(v, 0, one, 0) == 0);
  assert(mesh_api::peer_views_from_status(v, gone, one, 1) == 1);
  assert(std::string(one[0].fingerprint) == fp_hex_of(a.pub));
  std::printf("PASS test_the_peer_list_joins_each_member_from_the_view\n");
}

/* ── F199 — GET /api/mesh/peers lists the members the device holds ────────
 *
 * The rows are the published view's members, in the session's table order:
 * what the device enforces, so they always equal GET /api/mesh's
 * peers_total, and no store is read. The host's mesh_state stub lists no
 * stored pubkey (a board without flash encryption is refused the read and
 * lists none either), and every member still has its row; a member the
 * session drops leaves the list in the next pass, whatever NVS still holds;
 * a full table of eight lists eight. Before F199 the rows were the stored
 * pubkeys, so here there were none, beside peers_total 3 (and on a board
 * without flash encryption the route answered 500 load_failed). */
size_t row_count(const std::string& body) {
  size_t n = 0;
  for (size_t at = body.find("{\"fingerprint\""); at != std::string::npos;
       at = body.find("{\"fingerprint\"", at + 1)) {
    ++n;
  }
  return n;
}

void test_the_peer_list_is_the_members_the_device_holds() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x1C + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  TestMember a, b, c;
  const uint8_t mac_a[6] = {0x24, 0x0A, 0xC4, 0x1C, 0x00, 0x0A};
  const uint8_t mac_b[6] = {0x24, 0x0A, 0xC4, 0x1C, 0x00, 0x0B};
  const uint8_t mac_c[6] = {0x24, 0x0A, 0xC4, 0x1C, 0x00, 0x0C};
  make_member(a, mac_a);
  make_member(b, mac_b);
  make_member(c, mac_c, /*bind=*/false);   /* trusted, no address */
  member_speaks(a, S, 1000, -50);
  mesh_transport::test::set_now_ms(2000);
  mesh_transport::process();
  mesh_session::process(2000);

  /* The store the route read before F199 lists nothing here. */
  uint8_t stored[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  size_t n_stored = 99;
  (void)mesh_state::load_trusted_peers(stored, sizeof(stored), &n_stored);
  assert(n_stored == 0);

  const std::string rows = peers_body(2500);
  assert(row_count(rows) == 3 && has(status_body(), "\"peers_total\":3,"));
  assert(has(peer_row(2500, a.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":1,\"rssi\":-50,"
                                     "\"alerts_received\":1}"));
  assert(has(peer_row(2500, b.pub), "\"state\":\"OFFLINE\",\"last_seen_sec\":4294967295,\"rssi\":0,"));
  assert(has(peer_row(2500, c.pub), "\"state\":\"OFFLINE\",\"last_seen_sec\":4294967295,\"rssi\":0,"));
  assert(rows.find(fp_hex_of(a.pub)) < rows.find(fp_hex_of(b.pub)) &&
         rows.find(fp_hex_of(b.pub)) < rows.find(fp_hex_of(c.pub)));   /* table order */

  /* Dropped by the session (a removal whose NVS write failed leaves its
   * pubkey stored): gone from the list in the next pass. */
  assert(mesh_session::unregister_trusted_peer(b.fp));
  assert(!peer_row(2500, b.pub).empty());   /* the view stands until the pass */
  mesh_session::process(3000);
  assert(peer_row(3000, b.pub).empty());
  assert(row_count(peers_body(3000)) == 2 && has(status_body(), "\"peers_total\":2,"));

  /* A full table: eight members, eight rows, in the body's cap. */
  std::vector<TestMember> more(6);
  for (size_t i = 0; i < more.size(); ++i) {
    const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x1C, 0x01, (uint8_t)i};
    make_member(more[i], mac);
  }
  mesh_session::process(4000);
  assert(mesh_session::trusted_peer_count() == mesh_session::MAX_TRUSTED_PEERS);
  assert(row_count(peers_body(4000)) == mesh_session::MAX_TRUSTED_PEERS);
  assert(has(status_body(), "\"peers_total\":8,"));
  std::printf("PASS test_the_peer_list_is_the_members_the_device_holds\n");
}

/* Two threads, as on the device: the main loop starts or cancels a pairing
 * and renames the opera after it, then runs a pass, over and over; the
 * httpd task reads the status meanwhile. Each step's name says which step
 * it was: "R" and the pairing number for a start (the pairing running),
 * "C" and the number for a cancel (canceled), 32 characters either way. A
 * read that mixes two passes shows a name of two steps, or a name whose
 * letter or number is not the pairing it was read with. Every read through
 * read_status() is one pass. (Reading the live state instead mixes them
 * here on a multi-core host; the host lock is a std::mutex, so
 * ThreadSanitizer sees the real locking.) */
void test_status_reads_stay_whole_while_the_main_loop_runs() {
  uint8_t pub[32], priv[32];
  stand_up_session(nullptr, pub, priv);
  mesh_session::set_opera_name("C0000000000000000000000000000000");
  mesh_session::process(1);

  std::atomic<bool> done(false);
  std::atomic<long> reads(0), torn(0);
  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      mesh_session::StatusView v;
      mesh_session::read_status(&v);
      bool ok = std::strlen(v.opera_name) == mesh_pairing::MAX_OPERA_NAME_LEN;
      unsigned long n = 0;
      for (size_t i = 1; ok && i < mesh_pairing::MAX_OPERA_NAME_LEN; ++i) {
        const char c = v.opera_name[i];
        ok = c >= '0' && c <= '9';
        n = n * 10 + (unsigned long)(c - '0');
      }
      if (ok && v.opera_name[0] == 'R') {
        ok = n == v.pairing_seq && v.pairing_outcome == mesh_pairing::Outcome::RUNNING &&
             v.pairing_state == mesh_pairing::State::DISCOVERING_JOINER;
      } else if (ok && v.opera_name[0] == 'C') {
        ok = n == v.pairing_seq &&
             (n == 0 ? v.pairing_outcome == mesh_pairing::Outcome::NONE
                     : v.pairing_outcome == mesh_pairing::Outcome::FAILED &&
                       v.pairing_fail_reason == mesh_pairing::FailReason::CANCELED &&
                       v.pairing_state == mesh_pairing::State::FAILED);
      } else {
        ok = false;
      }
      if (!ok) torn.fetch_add(1);
      reads.fetch_add(1);
    }
  });
  uint32_t t = 10;
  char name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  for (int step = 0; step < 600; ++step) {
    if (step % 2 == 0) {
      assert(mesh_session::start_pairing_joiner(t));
    } else {
      mesh_session::cancel_pairing();
    }
    std::snprintf(name, sizeof(name), "%c%031lu", step % 2 == 0 ? 'R' : 'C',
                  (unsigned long)mesh_session::pairing_seq());
    mesh_session::set_opera_name(name);
    mesh_session::process(t);
    t += 1;
    if (g_outs.size() > 64) g_outs.clear();
  }
  done.store(true, std::memory_order_release);
  reader.join();
  assert(reads.load() > 0);
  if (torn.load() != 0) {
    std::printf("FAIL: %ld of %ld status reads mixed two passes\n", torn.load(), reads.load());
    std::fflush(stdout);
  }
  assert(torn.load() == 0);
  std::printf("PASS test_status_reads_stay_whole_while_the_main_loop_runs (%ld reads)\n",
              reads.load());
}

/* ── F324 — a periodic heartbeat, and liveness from verified frames only ──
 *
 * A running session with an opera sends the empty signed HEARTBEAT to its
 * bound members every HEARTBEAT_INTERVAL_MS +/- HEARTBEAT_JITTER_MS, every
 * heartbeat re-arming the deadline; a stopped pass disarms it. A member's
 * peer row (state, last_seen_sec, rssi) and GET /api/mesh's peers_online
 * come from its last frame that passed the signature, opera and counter
 * checks, never from the transport entry, which any frame from the address
 * refreshes. */

/* The two-Canary pairing, with the initiator's first PAIR_COMPLETE lost on
 * the air. The joiner pairs from the 2 s copy and its heartbeat ends the
 * copies, but the initiator's paired heartbeat reached it before it held
 * the opera, so it reads the initiator OFFLINE "never" until the
 * initiator's next heartbeat, which lands within one longest gap of the
 * paired one: from then on the joiner reads ACTIVE and the initiator
 * CONNECTED. Fails before F324: the joiner reads the initiator OFFLINE
 * "never" for the whole run. */
void test_a_lost_complete_is_healed_by_the_next_heartbeat() {
  using namespace two_canaries;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x6C + i);
  uint8_t i_pub[32], i_priv[32], j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(i_pub, i_priv));
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  Joiner joiner = fork_joiner(j_pub, j_priv);
  boot(i_pub, i_priv, S, 0);

  using RT = mesh_session::RequestType;
  mesh_session::RequestStatus st_i, st_j;
  std::vector<OutFrame> to_i, to_j, sent_i, sent_j;
  bool drop_complete = false;
  size_t dropped = 0;
  auto step = [&](uint32_t t, RT req_i, RT req_j) {
    sent_i = pass(MAC_J, to_i, t, req_i, req_i == RT::PAIR_START ? S : nullptr, &st_i);
    to_j = towards(sent_i, MAC_J);
    for (auto it = to_j.begin(); drop_complete && it != to_j.end(); ++it) {
      if (!it->bytes.empty() &&
          it->bytes[0] == static_cast<uint8_t>(mesh_session::MsgType::PAIR_COMPLETE)) {
        to_j.erase(it);
        drop_complete = false;
        ++dropped;
        break;
      }
    }
    sent_j = joiner.step(t, req_j, to_j, &st_j);
    to_i = towards(sent_j, MAC_I);
  };

  step(1000, RT::PAIR_START, RT::PAIR_JOIN);
  step(1100, RT::NONE, RT::NONE);
  step(1200, RT::NONE, RT::NONE);
  step(1300, RT::PAIR_CONFIRM, RT::PAIR_CONFIRM);
  drop_complete = true;
  step(1400, RT::NONE, RT::NONE);
  assert(dropped == 1);
  assert(g_paired_fired && mesh_session::trusted_peer_count() == 1);
  assert(count_type(sent_i, mesh_session::MsgType::PAIR_COMPLETE) == 1);
  assert(count_heartbeats(sent_i, MAC_J) == 1);   /* reaches a joiner not yet in the opera */
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t to_j_mac[1][6] = {{MAC_J[0], MAC_J[1], MAC_J[2], MAC_J[3], MAC_J[4], MAC_J[5]}};
  std::vector<OutFrame> paired_beat;
  for (const auto& f : sent_i) {
    if (is_heartbeat(f.bytes)) paired_beat.push_back(f);
  }
  const uint32_t paired_i =
      assert_only_member_heartbeats(paired_beat, i_pub, opera_id, to_j_mac, 1)[0].timestamp;

  /* The copy pairs the joiner; its heartbeat ends the copies. */
  size_t complete_copies = 0, joiner_beats = 0;
  uint32_t t = 1500;
  for (; t <= 1400 + 10000; t += 100) {
    step(t, RT::NONE, RT::NONE);
    complete_copies += count_type(sent_i, mesh_session::MsgType::PAIR_COMPLETE);
    joiner_beats += count_heartbeats(sent_j, MAC_I);
  }
  assert(complete_copies == 1 && joiner_beats == 1);
  std::string body_j, peers_j;
  joiner.report(t, &body_j, &peers_j);
  assert(has(body_j, "\"peers_total\":1,\"peers_online\":0"));
  assert(has(body_j, "\"state\":\"CONNECTING\""));
  assert(has(peers_j, "\"state\":\"OFFLINE\",\"last_seen_sec\":4294967295,"));
  mesh_transport::test::set_now_ms(t);
  assert(has(peers_body(t), "\"state\":\"CONNECTED\""));   /* the initiator heard the joiner */

  /* The initiator's next heartbeat heals it, within one longest gap. */
  const uint32_t pass_ms = 1000;
  const uint32_t heal_by = paired_i + beat_gap_max(pass_ms);
  std::vector<OutFrame> later_i;
  for (t = 12000; t <= heal_by; t += pass_ms) {
    step(t, RT::NONE, RT::NONE);
    later_i.insert(later_i.end(), sent_i.begin(), sent_i.end());
  }
  const std::vector<Beat> beats = assert_only_member_heartbeats(later_i, i_pub, opera_id, to_j_mac, 1);
  assert(beats.size() == 1);
  assert(beats[0].timestamp - paired_i >= kBeatGapMin);
  joiner.report(heal_by, &body_j, &peers_j);
  assert(has(body_j, "\"state\":\"ACTIVE\""));
  assert(has(body_j, "\"peers_total\":1,\"peers_online\":1"));
  assert(has(peers_j, "\"state\":\"CONNECTED\",\"last_seen_sec\":"));
  assert(last_seen_sec_of(peers_j) <= (heal_by - beats[0].timestamp) / 1000);
  joiner.quit();
  std::printf("PASS test_a_lost_complete_is_healed_by_the_next_heartbeat"
              "  (healed %u ms after the paired heartbeat)\n",
              (unsigned)(beats[0].timestamp - paired_i));
}

/* Every heartbeat re-arms the deadline, not only the periodic one. A
 * session with a member bound arms it on its first pass; a pairing that
 * completes 19 s later sends its paired heartbeat (F162) to both members,
 * and the next one comes no sooner than kBeatGapMin after THAT, as one
 * round to both, within one longest gap. A timer that only its own sends
 * re-armed would fire 8-14 s after the paired one. Fails before F324 (no
 * periodic heartbeat at all) and with the arm moved out of
 * send_heartbeat() into the periodic branch. */
void test_every_heartbeat_re_arms_the_timer() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xB5 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  reset_fake_main_nvs();
  mesh_session::set_paired_callback(main_like_paired);
  mesh_session::set_paired_peer_bound_callback(main_like_bound);
  const uint8_t me[6]    = {0x24, 0x0A, 0xC4, 0x32, 0x80, 0x01};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x32, 0x80, 0x02};
  const uint8_t mac_k[6] = {0x24, 0x0A, 0xC4, 0x32, 0x80, 0x0B};
  add_bound_member(mac_k);
  const uint32_t first_pass = 1000, T0 = 20000, pass_ms = 500;
  assert(T0 - first_pass < kBeatGapMin);   /* the pairing lands before the first deadline */
  for (uint32_t t = first_pass; t < T0; t += pass_ms) mesh_session::process(t);
  assert(g_outs.empty());
  uint8_t j_pub[32], j_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(j_pub, j_priv));
  mesh_pairing::PairingContext cj;
  initiator_completes(S, me, mac_j, j_pub, j_priv, T0, cj);   /* the paired heartbeat, both */
  const uint32_t paired = T0 + 10;                             /* rest_confirm's pass */
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t members[2][6] = {{mac_k[0], mac_k[1], mac_k[2], mac_k[3], mac_k[4], mac_k[5]},
                                 {mac_j[0], mac_j[1], mac_j[2], mac_j[3], mac_j[4], mac_j[5]}};
  std::vector<OutFrame> beats_out;
  for (uint32_t t = paired + pass_ms; t <= paired + beat_gap_max(pass_ms); t += pass_ms) {
    g_outs.clear();
    mesh_session::process(t);
    for (const auto& o : g_outs) {
      if (is_heartbeat(o.bytes)) beats_out.push_back(o);   /* the COMPLETE copies aside */
    }
  }
  const std::vector<Beat> beats = assert_only_member_heartbeats(beats_out, pub, opera_id, members, 2);
  assert(beats.size() == 2 && beats[0].counter == beats[1].counter);   /* one round, to both */
  assert(beats[0].timestamp - paired >= kBeatGapMin);
  mesh_session::cancel_pairing();
  std::printf("PASS test_every_heartbeat_re_arms_the_timer  (%u ms after the paired one)\n",
              (unsigned)(beats[0].timestamp - paired));
}

/* An hour of passes in each state that must send nothing: a session
 * initialized but never started, one turned off (set_enabled(false)), one
 * with no opera, one with an opera and no bound member. Not one frame and
 * not one counter in any of them (F101 and F162's no-counter rule, now on a
 * timer). Then the missing piece is supplied (started, turned on, the
 * opera set, the member bound) and a heartbeat goes to the member within
 * one longest gap: exactly one after a start or an enable (the stopped
 * passes disarmed the timer, so the first running pass arms it afresh),
 * and the first within one longest gap after an opera or a binding (the
 * timer kept re-checking once an interval). The positive halves fail
 * before F324 (no periodic send); the guard halves pin that the timer
 * spends nothing it may not. */
void test_no_periodic_heartbeat_without_a_running_opera() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x7D + i);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0x40, 0x01}};
  const uint32_t pass_ms = 1000, hour = 3600000;
  enum Case { NOT_STARTED, DISABLED, NO_OPERA, NO_BOUND_MEMBER };
  const char* names[] = {"not started", "disabled", "no opera", "no bound member"};
  for (int c = NOT_STARTED; c <= NO_BOUND_MEMBER; ++c) {
    uint8_t pub[32], priv[32];
    stand_up_session(c == NO_OPERA ? nullptr : S, pub, priv);
    if (c == NOT_STARTED) {
      /* Initialized, never started: the session as init() leaves it. */
      mesh_session::deinit();
      assert(mesh_session::init(pub, priv));
      assert(mesh_session::set_opera_secret(S));
      assert(!mesh_session::is_running());
    }
    uint8_t m_pub[32], m_priv[32], m_fp[8];
    assert(mesh_crypto::ed25519_generate_keypair(m_pub, m_priv));
    assert(mesh_session::register_trusted_peer(m_pub));
    mesh_crypto::compute_fingerprint(m_pub, m_fp);
    if (c != NO_BOUND_MEMBER) assert(mesh_session::bind_peer_mac(m_fp, mac[0]));
    if (c == DISABLED) mesh_session::set_enabled(false);
    const uint64_t ctr0 = mesh_session::outbound_counter();
    g_outs.clear();
    for (uint32_t t = pass_ms; t <= hour; t += pass_ms) {
      mesh_transport::test::set_now_ms(t);
      mesh_session::process(t);
    }
    assert(g_outs.empty() && mesh_session::outbound_counter() == ctr0);

    const uint32_t E = hour + pass_ms;
    switch (c) {
      case NOT_STARTED:     assert(mesh_session::start()); break;
      case DISABLED:        mesh_session::set_enabled(true); break;
      case NO_OPERA:        assert(mesh_session::set_opera_secret(S)); break;
      case NO_BOUND_MEMBER: assert(mesh_session::bind_peer_mac(m_fp, mac[0])); break;
    }
    std::vector<uint32_t> at;
    for (uint32_t t = E; t <= E + beat_gap_max(pass_ms); t += pass_ms) {
      mesh_transport::test::set_now_ms(t);
      g_outs.clear();
      mesh_session::process(t);
      for (const auto& b : assert_only_member_heartbeats(g_outs, pub, opera_id, mac, 1)) {
        at.push_back(b.timestamp);
      }
    }
    assert(!at.empty() && at[0] - E < beat_gap_max(pass_ms));
    if (c == NOT_STARTED || c == DISABLED) {
      assert(at.size() == 1 && at[0] - E >= kBeatGapMin);
    }
    assert(mesh_session::outbound_counter() == ctr0 + at.size());
    std::printf("  F324 %-15s: an hour silent, then a heartbeat %u ms after\n", names[c],
                (unsigned)(at[0] - E));
  }
  std::printf("PASS test_no_periodic_heartbeat_without_a_running_opera\n");
}

/* A disable that spans more than 2^31 ms of the loop's clock. A deadline
 * armed before it, compared after it, reads as about 24.8 days in the
 * future (the signed difference wraps), so a session that kept it would
 * send nothing for that long. The stopped passes disarm it, and the first
 * running pass after the enable arms it afresh: exactly one heartbeat in
 * the next longest gap. Fails before F324 (no timer) and without the
 * stopped pass's disarm (none in that window). */
void test_the_heartbeat_timer_survives_a_long_disable() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x8E + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0x50, 0x01}};
  add_bound_member(mac[0]);
  mesh_session::process(1000);   /* the first running pass arms the deadline */
  assert(g_outs.empty());
  mesh_session::set_enabled(false);
  const uint32_t t0 = 1000 + beat_gap_max(1000) + 5000;   /* past any deadline armed at 1000 */
  mesh_session::process(t0);
  const uint32_t t1 = t0 + 0x80000000u + 5000;
  mesh_transport::test::set_now_ms(t1);
  mesh_session::process(t1);
  assert(g_outs.empty());
  mesh_session::set_enabled(true);
  std::vector<uint32_t> at;
  for (uint32_t t = t1; t - t1 <= beat_gap_max(1000); t += 1000) {
    mesh_transport::test::set_now_ms(t);
    g_outs.clear();
    mesh_session::process(t);
    for (const auto& b : assert_only_member_heartbeats(g_outs, pub, opera_id, mac, 1)) {
      at.push_back(b.timestamp);
    }
  }
  assert(at.size() == 1 && at[0] - t1 >= kBeatGapMin);
  std::printf("PASS test_the_heartbeat_timer_survives_a_long_disable  (%u ms after the enable)\n",
              (unsigned)(at[0] - t1));
}

/* One verified frame from a member at rssi -40, then once a minute for ten
 * minutes, all from the member's own address at rssi -90: a 3-byte junk
 * frame, a byte-identical replay of that verified frame, a full-length
 * heartbeat with a fresh counter and one signature byte flipped (it fails
 * the signature check, step 4), and one the member signed with a fresh
 * counter under another opera's secret (it fails the opera check, step 5).
 * None passes the checks, so none refreshes anything: the member reads
 * STALE after 90 s and OFFLINE after 5 minutes, with the verified frame's
 * age and RSSI, and the opera reads CONNECTING with peers_online 0. A fresh
 * verified frame then reads it CONNECTED at once. Fails before F324: the
 * transport stamp the junk refreshed made it CONNECTED, last_seen_sec 1,
 * rssi -90, and the status ACTIVE with peers_online 1 (probed at 3e593a01);
 * and on a stamp in the signature or the opera check's failure branch. */
void test_liveness_counts_only_verified_frames() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x9F + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  TestMember m;
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0x60, 0x01};
  make_member(m, mac);
  uint8_t verified[mesh_envelope::MAX_FRAME_LEN];
  const size_t vlen = build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                                 mesh_envelope::MsgType::HEARTBEAT, nullptr, 0,
                                                 verified, sizeof(verified));
  assert(vlen == mesh_envelope::MIN_FRAME_LEN);
  mesh_transport::test::set_now_ms(1000);
  mesh_transport::test::inject_recv(mac, verified, vlen, -40);
  mesh_transport::process();
  mesh_session::process(1000);
  assert(has(peer_row(1000, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,\"rssi\":-40,"));
  assert(has(status_body(), "\"state\":\"ACTIVE\"") &&
         has(status_body(), "\"peers_online\":1,"));

  /* A round a minute from 61000 to 601000; at the second round's pass the
   * verified frame is past STALE and short of OFFLINE. */
  const uint8_t junk[3] = {mesh_envelope::OPERA_VERSION, 0xEE, 0xEE};
  uint8_t other_opera[32];
  for (size_t i = 0; i < sizeof(other_opera); ++i) other_opera[i] = (uint8_t)(S[i] ^ 0x5A);
  const uint32_t round_ms = 60000, last_round = 601000, read_at = 602000;
  const uint32_t stale_at = 1000 + 2 * round_ms;
  assert(stale_at - 1000 >= mesh_transport::PEER_STALE_AFTER_MS &&
         stale_at - 1000 < mesh_transport::PEER_OFFLINE_AFTER_MS);
  bool saw_stale = false;
  for (uint32_t t = 2000; t <= read_at; t += 1000) {
    mesh_transport::test::set_now_ms(t);
    if (t <= last_round && (t - 1000) % round_ms == 0) {
      mesh_transport::test::inject_recv(mac, junk, sizeof(junk), -90);
      mesh_transport::test::inject_recv(mac, verified, vlen, -90);   /* the replay */
      uint8_t forged[mesh_envelope::MAX_FRAME_LEN];
      const size_t forged_len = build_signed_session_frame(
          m.pub, m.priv, S, ++m.counter, mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, forged,
          sizeof(forged));
      assert(forged_len == mesh_envelope::MIN_FRAME_LEN);
      forged[forged_len - 1] ^= 0x01;   /* the signature's last byte */
      mesh_transport::test::inject_recv(mac, forged, forged_len, -90);
      uint8_t foreign[mesh_envelope::MAX_FRAME_LEN];
      const size_t foreign_len = build_signed_session_frame(
          m.pub, m.priv, other_opera, ++m.counter, mesh_envelope::MsgType::HEARTBEAT, nullptr, 0,
          foreign, sizeof(foreign));
      assert(foreign_len == mesh_envelope::MIN_FRAME_LEN);
      mesh_transport::test::inject_recv(mac, foreign, foreign_len, -90);
    }
    mesh_transport::process();
    mesh_session::process(t);
    if (t == stale_at) {
      const std::string row = peer_row(t, m.pub);
      assert(has(row, "\"state\":\"STALE\",") && has(row, "\"rssi\":-40,"));
      assert(last_seen_sec_of(row) == (stale_at - 1000) / 1000);
      assert(has(status_body(), "\"state\":\"CONNECTING\"") &&
             has(status_body(), "\"peers_online\":0,"));
      saw_stale = true;
    }
  }
  assert(saw_stale);
  /* The transport saw every frame: its entry is fresh, and unread. */
  mesh_transport::Peer tp;
  assert(mesh_transport::get_peer(mac, &tp) && tp.rssi_dbm == -90 &&
         tp.state == mesh_transport::PeerState::ACTIVE);
  const std::string row = peer_row(read_at, m.pub);
  assert(has(row, "\"state\":\"OFFLINE\",") && has(row, "\"rssi\":-40,"));
  assert(last_seen_sec_of(row) == (read_at - 1000) / 1000);   /* the verified frame's age */
  assert(has(status_body(), "\"state\":\"CONNECTING\"") &&
         has(status_body(), "\"peers_total\":1,\"peers_online\":0,"));

  /* A fresh verified frame: CONNECTED at once, its own RSSI. */
  uint8_t fresh[mesh_envelope::MAX_FRAME_LEN];
  const size_t flen = build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                                 mesh_envelope::MsgType::HEARTBEAT, nullptr, 0,
                                                 fresh, sizeof(fresh));
  mesh_transport::test::inject_recv(mac, fresh, flen, -45);
  mesh_transport::process();
  mesh_session::process(read_at);
  assert(has(peer_row(read_at, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,\"rssi\":-45,"));
  assert(has(status_body(), "\"state\":\"ACTIVE\"") &&
         has(status_body(), "\"peers_online\":1,"));
  std::printf("PASS test_liveness_counts_only_verified_frames\n");
}

/* A member gone for longer than the 32-bit clock can count. Its age is an
 * unsigned difference, so 2^32 ms (about 49.7 days) after its last
 * verified frame it would read 0 again: CONNECTED and online, with nothing
 * received. Every pass holds a heard member's age at 2^31 ms or under, so
 * it stays OFFLINE, last seen about 24.8 days ago, and the opera
 * CONNECTING. Passes here are 2^20 ms apart, far wider than the loop's.
 * Fails before F324 (the transport entry's age wrapped: CONNECTED 10 s ago,
 * peers_online 1) and on the verified stamp without the cap. */
void test_a_long_gone_member_stays_offline_past_the_clock_wrap() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xA4 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  TestMember m;
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0x70, 0x01};
  make_member(m, mac);
  member_speaks(m, S, 1000, -40);
  mesh_session::process(1000);
  assert(has(peer_row(1000, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  const uint32_t step_ms = 1u << 20;
  uint32_t t = 1000;
  for (uint64_t k = 0; k < (1ull << 32) / step_ms; ++k) {
    t += step_ms;
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  }
  assert(t == 1000);   /* the clock went all the way round */
  t += 10000;          /* 10 s past the verified frame's own instant, 2^32 ms later */
  mesh_transport::test::set_now_ms(t);
  mesh_transport::process();
  mesh_session::process(t);
  const std::string row = peer_row(t, m.pub);
  assert(has(row, "\"state\":\"OFFLINE\",") && has(row, "\"rssi\":-40,"));
  assert(last_seen_sec_of(row) >= 0x7FFFFFFFu / 1000u);   /* about 24.8 days, held there */
  assert(has(status_body(), "\"state\":\"CONNECTING\"") &&
         has(status_body(), "\"peers_online\":0,"));
  member_speaks(m, S, t, -44);
  mesh_session::process(t);
  assert(has(peer_row(t, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,\"rssi\":-44,"));

  /* The same wrap with the mesh turned off. A stopped session still
   * publishes its peer rows every pass (F161), so the stopped passes hold
   * the age too; the cap sits before the stopped pass's return, not after
   * it. Without that the row read CONNECTED again 2^32 ms into a disable. */
  mesh_session::set_enabled(false);
  const uint32_t off_at = t;
  for (uint64_t k = 0; k < (1ull << 32) / step_ms; ++k) {
    t += step_ms;
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  }
  assert(t == off_at);
  t += 10000;
  mesh_transport::test::set_now_ms(t);
  mesh_transport::process();
  mesh_session::process(t);
  assert(!mesh_session::is_running());
  const std::string off_row = peer_row(t, m.pub);
  assert(has(off_row, "\"state\":\"OFFLINE\",") && has(off_row, "\"rssi\":-44,"));
  assert(last_seen_sec_of(off_row) >= 0x7FFFFFFFu / 1000u);
  mesh_session::set_enabled(true);
  std::printf("PASS test_a_long_gone_member_stays_offline_past_the_clock_wrap"
              "  (running and stopped)\n");
}

/* The deadline across the 32-bit clock's wrap. A device up about 49.7 days
 * arms a deadline past 2^32 ms, which wraps to a small number; a plain
 * `now >= deadline` reads that as due at once and signs a heartbeat on
 * every pass until the clock wraps too (up to about 33 s of them: an
 * Ed25519 signature, a counter and a send per member per pass). The signed
 * difference reads it right. 200 s of 1 s passes either side of the wrap,
 * the transport clock alongside: only member heartbeats, one gap spanning
 * the wrap, every gap within the bounds and their number within what the
 * bounds allow. */
void test_the_heartbeat_spacing_holds_across_the_millis_wrap() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xC3 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0x90, 0x01}};
  add_bound_member(mac[0]);
  const uint32_t pass_ms = 1000, span = 200000;
  const uint32_t w0 = 0u - span, w1 = span;   /* 2^32 - 200 s to 200 s past the wrap */
  std::vector<OutFrame> outs;
  for (uint32_t t = w0;; t += pass_ms) {
    mesh_transport::test::set_now_ms(t);
    g_outs.clear();
    mesh_session::process(t);   /* the first pass, at w0, arms the deadline */
    outs.insert(outs.end(), g_outs.begin(), g_outs.end());
    if (t == w1) break;
  }
  const std::vector<Beat> beats = assert_only_member_heartbeats(outs, pub, opera_id, mac, 1);
  const std::vector<uint32_t> times = beat_times_to(beats, mac[0]);
  bool spans_the_wrap = false;
  for (size_t k = 1; k < times.size(); ++k) spans_the_wrap |= times[k] < times[k - 1];
  assert(spans_the_wrap);
  size_t n_min = 0, n_max = 0;
  check_beat_spacing(times, w0, w0 + pass_ms, w1, pass_ms, &n_min, &n_max);
  std::printf("PASS test_the_heartbeat_spacing_holds_across_the_millis_wrap"
              "  (%zu heartbeats in %u s, %zu-%zu allowed)\n",
              times.size(), (unsigned)((w1 - w0) / 1000), n_min, n_max);
}

/* The jitter is drawn for every interval, not once. Over an hour of 100 ms
 * passes the gaps between one member's heartbeats spread over at least half
 * the jitter's range (HEARTBEAT_JITTER_MS of the 2 * HEARTBEAT_JITTER_MS)
 * and take at least a quarter of the values a gap can take at that pass
 * granularity. A fixed deadline (the interval, or either end of the range)
 * or one draw kept for good keeps every gap inside the bounds the other
 * tests hold, and gives one value here, so two Canaries whose timers met
 * would beat in step from then on. The host's fill_random is the seeded
 * rand(), so the run is the same every time. */
void test_each_heartbeat_deadline_is_drawn_afresh() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xD6 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0xA0, 0x01}};
  add_bound_member(mac[0]);
  const uint32_t pass_ms = 100, first = 1000, last = first + 3600000;
  std::vector<OutFrame> outs;
  for (uint32_t t = first; t <= last; t += pass_ms) {
    mesh_transport::test::set_now_ms(t);
    g_outs.clear();
    mesh_session::process(t);
    outs.insert(outs.end(), g_outs.begin(), g_outs.end());
  }
  const std::vector<uint32_t> times =
      beat_times_to(assert_only_member_heartbeats(outs, pub, opera_id, mac, 1), mac[0]);
  size_t n_min = 0, n_max = 0;
  check_beat_spacing(times, first, first + pass_ms, last, pass_ms, &n_min, &n_max);
  std::vector<uint32_t> gaps;
  for (size_t k = 1; k < times.size(); ++k) gaps.push_back(times[k] - times[k - 1]);
  uint32_t lo = UINT32_MAX, hi = 0;
  std::vector<uint32_t> distinct;
  for (uint32_t g : gaps) {
    lo = g < lo ? g : lo;
    hi = g > hi ? g : hi;
    bool seen = false;
    for (uint32_t d : distinct) seen |= d == g;
    if (!seen) distinct.push_back(g);
  }
  /* A gap is a whole number of passes between kBeatGapMin and the longest
   * draw, so it can take this many values. */
  const size_t values = 2u * mesh_session::HEARTBEAT_JITTER_MS / pass_ms + 1u;
  assert(hi - lo >= mesh_session::HEARTBEAT_JITTER_MS);
  assert(distinct.size() >= values / 4);
  std::printf("PASS test_each_heartbeat_deadline_is_drawn_afresh"
              "  (%zu gaps, %u-%u ms, %zu distinct of %zu possible)\n",
              gaps.size(), (unsigned)lo, (unsigned)hi, distinct.size(), values);
}

/* send_heartbeat() re-arms the deadline before its guards, so a session
 * with an opera and no member bound re-checks once an interval instead of
 * keeping one deadline that ages. Kept past 2^31 ms, that deadline would
 * read as weeks ahead (the signed difference wraps), and the first member
 * bound without a heartbeat of its own would then wait up to about 24.8
 * days for one. Every path that binds a member today (a pairing, the boot
 * restore) sends one, which re-arms anyway; this pins the order for any
 * path that does not. No member for 2^31 ms and more of 20 s passes, then
 * one bound directly: a heartbeat reaches it within one longest gap. */
void test_a_member_bound_after_weeks_alone_is_heard_within_one_interval() {
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xE4 + i);
  uint8_t pub[32], priv[32];
  stand_up_session(S, pub, priv);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0xB0, 0x01}};
  const uint32_t alone_pass_ms = 20000, first = 1000;
  const uint32_t alone_until = first + 0x80000000u + beat_gap_max(alone_pass_ms);
  const uint64_t ctr0 = mesh_session::outbound_counter();
  uint32_t t = first;
  for (; t - first < alone_until - first; t += alone_pass_ms) {
    mesh_transport::test::set_now_ms(t);
    mesh_session::process(t);
  }
  assert(g_outs.empty() && mesh_session::outbound_counter() == ctr0);
  add_bound_member(mac[0]);
  const uint32_t bound_at = t, pass_ms = 1000;
  std::vector<uint32_t> at;
  for (t = bound_at + pass_ms; t - bound_at <= beat_gap_max(pass_ms); t += pass_ms) {
    mesh_transport::test::set_now_ms(t);
    g_outs.clear();
    mesh_session::process(t);
    for (const auto& b : assert_only_member_heartbeats(g_outs, pub, opera_id, mac, 1)) {
      at.push_back(b.timestamp);
    }
  }
  assert(!at.empty());
  std::printf("PASS test_a_member_bound_after_weeks_alone_is_heard_within_one_interval"
              "  (%u ms after the bind)\n", (unsigned)(at[0] - bound_at));
}

/* What verified-only liveness does not cover, pinned so the figures the
 * docs give come from a test (THREAT_MODEL, spec §8.3). The counter check
 * is a high-water mark, so any genuine frame of a member's that the
 * receiver did not hear verifies whenever it arrives from the member's own
 * address, and stamps the member live: a recorder that sends such frames
 * one each just inside PEER_STALE_AFTER_MS holds a member that is gone
 * CONNECTED about 90 s per frame. There is no fixed bound; the hold is the
 * size of the recorder's stock. The legs pin where a stock comes from:
 *  - Leg 1, a reboot. The receiver's replay counters come back from
 *    main.cpp's 5-minute save (kReplaySaveMs below), so every frame it heard
 *    since that save is fresh again. A member heartbeating at the shortest
 *    spacing sends at most (kReplaySaveMs - 1) / kBeatGapMin + 1 of them
 *    between two saves (12, about 18 minutes held). That figure is for a
 *    reboot with no downtime; leg 3's stock adds to it. The frame at the
 *    saved counter refreshes nothing, and before the reboot none of them
 *    does.
 *  - Leg 2, a jammer. It keeps the member's frames from the receiver and
 *    sends them late: the same, for as long as its stock lasts, with no
 *    STALE between them. One frame the receiver hears from the member
 *    itself makes every older one a replay.
 *  - Leg 3, downtime. A member heartbeats a bound member's address every
 *    interval whether or not that member is listening, so every heartbeat
 *    it sends while the receiver is off, booting, out of range or on
 *    another channel is one the receiver never heard, and a recorder near
 *    the member keeps them with no jamming. A real session runs for an hour
 *    as the member; the receiver comes back with the counter it saved just
 *    before the cut and the member gone, and the hour's heartbeats, relayed,
 *    hold it CONNECTED about (PEER_STALE_AFTER_MS - pass) /
 *    HEARTBEAT_INTERVAL_MS times the downtime: about three hours for one
 *    hour down. Before F324 a member sent nothing on a timer, so a downtime
 *    left no frames to keep.
 * A receiver that reboots before its first save after a pairing held no
 * counter for the member at all; test_a_members_first_counter_is_saved_at_once
 * pins the save that narrows that one, and
 * test_an_older_first_frame_is_saved_over_once_more what it leaves open. */
void test_a_frame_the_receiver_has_not_heard_still_counts_as_live() {
  /* main.cpp loop(): persist_replay_counters() every 300000 ms. */
  constexpr uint32_t kReplaySaveMs = 300000;
  constexpr uint32_t kMaxUnsaved   = (kReplaySaveMs - 1) / kBeatGapMin + 1;
  const uint32_t pass_ms = 1000;
  const uint32_t step = mesh_transport::PEER_STALE_AFTER_MS - pass_ms;   /* one just inside */
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0xF1 + i);
  uint8_t pub[32], priv[32];
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0xC0, 0x01};
  auto heartbeat = [&](TestMember& m) {
    std::vector<uint8_t> f(mesh_envelope::MAX_FRAME_LEN);
    f.resize(build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                        mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, f.data(),
                                        f.size()));
    assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
    return f;
  };
  auto deliver = [&](const std::vector<uint8_t>& f, uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::test::inject_recv(mac, f.data(), f.size(), -60);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto idle = [&](uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  };
  /* Sends `stock` from `r0`, one every `step`, and reads every pass until
   * the member reads OFFLINE: CONNECTED, the opera ACTIVE with one online,
   * until PEER_STALE_AFTER_MS after the last frame, then STALE until
   * PEER_OFFLINE_AFTER_MS after it. Returns how long it read CONNECTED. */
  auto relay = [&](const TestMember& m, const std::vector<std::vector<uint8_t>>& stock,
                   uint32_t r0) {
    const uint32_t last = r0 + (uint32_t)(stock.size() - 1) * step;
    uint32_t connected_ms = 0;
    for (uint32_t t = r0; t <= last + mesh_transport::PEER_OFFLINE_AFTER_MS; t += pass_ms) {
      if (t <= last && (t - r0) % step == 0) deliver(stock[(t - r0) / step], t);
      else idle(t);
      const std::string row = peer_row(t, m.pub);
      if (t < last + mesh_transport::PEER_STALE_AFTER_MS) {
        assert(has(row, "\"state\":\"CONNECTED\",") && has(row, "\"rssi\":-60,"));
        assert(has(status_body(), "\"state\":\"ACTIVE\"") &&
               has(status_body(), "\"peers_online\":1,"));
        connected_ms += pass_ms;
      } else if (t < last + mesh_transport::PEER_OFFLINE_AFTER_MS) {
        assert(has(row, "\"state\":\"STALE\","));
      } else {
        assert(has(row, "\"state\":\"OFFLINE\","));
      }
    }
    return connected_ms;
  };

  /* Leg 1: a receiver reboot. */
  stand_up_session(S, pub, priv);
  TestMember m;
  make_member(m, mac);
  const uint32_t saved_at = 1000;
  const std::vector<uint8_t> f_saved = heartbeat(m);
  deliver(f_saved, saved_at);
  assert(has(peer_row(saved_at, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  uint8_t fps[mesh_session::MAX_REPLAY_COUNTERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs[mesh_session::MAX_REPLAY_COUNTERS];
  assert(mesh_session::get_replay_counters(fps, ctrs, mesh_session::MAX_REPLAY_COUNTERS) == 1);
  const uint64_t saved = ctrs[0];   /* main.cpp's save, just after this frame */
  assert(saved == m.counter);
  std::vector<std::vector<uint8_t>> heard;   /* the shortest spacing, until the next save */
  uint32_t t = saved_at + pass_ms;
  for (uint32_t k = 0; k < kMaxUnsaved; ++k, t += kBeatGapMin) {
    assert(t - saved_at < kReplaySaveMs);
    heard.push_back(heartbeat(m));
    deliver(heard.back(), t);
    assert(has(peer_row(t, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  }
  /* The member is gone. Before the reboot every one of them is a replay. */
  const uint32_t gone_at = t - kBeatGapMin;
  for (const auto& f : heard) deliver(f, gone_at + mesh_transport::PEER_STALE_AFTER_MS);
  assert(has(peer_row(gone_at + mesh_transport::PEER_STALE_AFTER_MS, m.pub),
             "\"state\":\"STALE\","));
  /* The reboot, before the next save: main.cpp's boot restore registers the
   * member, binds its stored address, then restores the saved counter. */
  stand_up_session(S, pub, priv);
  assert(mesh_session::register_trusted_peer(m.pub));
  assert(mesh_session::bind_peer_mac(m.fp, mac));
  assert(mesh_session::restore_replay_counter(m.fp, saved));
  const uint32_t r0 = gone_at + 2 * mesh_transport::PEER_STALE_AFTER_MS;
  deliver(f_saved, r0 - pass_ms);   /* at the saved counter: refreshes nothing */
  assert(has(peer_row(r0 - pass_ms, m.pub), "\"state\":\"OFFLINE\",") &&
         has(status_body(), "\"peers_online\":0,"));
  const uint32_t held_ms = relay(m, heard, r0);
  assert(held_ms <= kMaxUnsaved * mesh_transport::PEER_STALE_AFTER_MS);
  assert(held_ms == (kMaxUnsaved - 1) * step + mesh_transport::PEER_STALE_AFTER_MS);

  /* Leg 2: a jammer. The member is heard once, then its next frames never
   * reach the receiver; sent late, each holds it CONNECTED. Half the stock,
   * then one frame the receiver hears from the member itself, and the rest
   * of the stock refreshes nothing. */
  stand_up_session(S, pub, priv);
  TestMember j;
  make_member(j, mac);
  deliver(heartbeat(j), 1000);
  const uint32_t stock_n = 6;
  std::vector<std::vector<uint8_t>> stock;
  for (uint32_t k = 0; k < stock_n; ++k) stock.push_back(heartbeat(j));   /* jammed */
  const uint32_t j0 = 1000 + mesh_transport::PEER_STALE_AFTER_MS - pass_ms;
  const std::vector<std::vector<uint8_t>> first_half(stock.begin(), stock.begin() + stock_n / 2);
  const uint32_t jammed_ms = relay(j, first_half, j0);
  assert(jammed_ms == (stock_n / 2 - 1) * step + mesh_transport::PEER_STALE_AFTER_MS);
  const uint32_t own_at = j0 + (stock_n / 2 - 1) * step + mesh_transport::PEER_OFFLINE_AFTER_MS +
                          pass_ms;
  deliver(heartbeat(j), own_at);   /* the member itself, heard */
  assert(has(peer_row(own_at, j.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  for (uint32_t k = stock_n / 2; k < stock_n; ++k) {
    const uint32_t at = own_at + (k - stock_n / 2 + 1) * step;
    deliver(stock[k], at);   /* older than the member's own: a replay now */
    assert(last_seen_sec_of(peer_row(at, j.pub)) == (at - own_at) / 1000);
  }
  const uint32_t after = own_at + mesh_transport::PEER_OFFLINE_AFTER_MS;
  idle(after);
  assert(has(peer_row(after, j.pub), "\"state\":\"OFFLINE\","));

  /* Leg 3: downtime. This session is the member now, with the receiver
   * bound at rx_mac, and runs for kDownMs past its first heartbeat, which
   * the receiver heard and saved just before it went down. */
  constexpr uint32_t kDownMs = 3600000;
  uint8_t mpub[32], mpriv[32];
  stand_up_session(S, mpub, mpriv);
  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(S, opera_id);
  const uint8_t rx_mac[1][6] = {{0x24, 0x0A, 0xC4, 0x32, 0xC0, 0x02}};
  add_bound_member(rx_mac[0]);
  std::vector<OutFrame> sent;
  uint32_t first_beat = 0;
  for (uint32_t d = 1000;; d += pass_ms) {
    mesh_transport::test::set_now_ms(d);
    g_outs.clear();
    mesh_session::process(d);
    sent.insert(sent.end(), g_outs.begin(), g_outs.end());
    if (first_beat == 0 && !sent.empty()) first_beat = d;
    if (first_beat != 0 && d - first_beat >= kDownMs) break;
  }
  const std::vector<Beat> beats = assert_only_member_heartbeats(sent, mpub, opera_id, rx_mac, 1);
  const std::vector<uint32_t> times = beat_times_to(beats, rx_mac[0]);
  assert(times.size() == sent.size() && times[0] == first_beat);
  const std::vector<uint32_t> down_times(times.begin() + 1, times.end());
  size_t n_min = 0, n_max = 0;
  check_beat_spacing(down_times, first_beat, first_beat + pass_ms, first_beat + kDownMs, pass_ms,
                     &n_min, &n_max);
  std::vector<std::vector<uint8_t>> unheard;
  for (size_t k = 1; k < sent.size(); ++k) unheard.push_back(sent[k].bytes);
  /* The receiver comes back with the member gone: main.cpp's boot restore
   * registers the member, binds its stored address, restores the save. */
  TestMember i;
  std::memcpy(i.pub, mpub, sizeof(i.pub));
  mesh_crypto::compute_fingerprint(i.pub, i.fp);
  stand_up_session(S, pub, priv);
  assert(mesh_session::register_trusted_peer(i.pub));
  assert(mesh_session::bind_peer_mac(i.fp, mac));
  assert(mesh_session::restore_replay_counter(i.fp, beats[0].counter));
  const uint32_t d0 = 2 * kDownMs;
  deliver(sent[0].bytes, d0 - pass_ms);   /* at the saved counter: refreshes nothing */
  assert(has(peer_row(d0 - pass_ms, i.pub), "\"state\":\"OFFLINE\",") &&
         has(status_body(), "\"peers_online\":0,"));
  const uint32_t down_ms = relay(i, unheard, d0);
  assert(down_ms == (uint32_t)(unheard.size() - 1) * step + mesh_transport::PEER_STALE_AFTER_MS);
  /* So the hold is `step` per heartbeat the member sent while the receiver
   * was down, about (STALE - pass) / INTERVAL times the downtime: between
   * what every gap at its longest and every gap at its shortest give (the
   * counts check_beat_spacing derived), and more than twice the downtime
   * either way. */
  assert(down_ms >= (uint32_t)(n_min - 1) * step + mesh_transport::PEER_STALE_AFTER_MS);
  assert(down_ms <= (uint32_t)(n_max - 1) * step + mesh_transport::PEER_STALE_AFTER_MS);
  assert(down_ms > 2 * kDownMs);
  std::printf("PASS test_a_frame_the_receiver_has_not_heard_still_counts_as_live"
              "  (a reboot: %u frames since the save held a gone member CONNECTED %u s;"
              " a jammer: %u frames, %u s; %u s down: %zu frames, %u s, %.2f times the downtime)\n",
              (unsigned)kMaxUnsaved, (unsigned)(held_ms / 1000), (unsigned)(stock_n / 2),
              (unsigned)(jammed_ms / 1000), (unsigned)(kDownMs / 1000), unheard.size(),
              (unsigned)(down_ms / 1000), (double)down_ms / kDownMs);
}

/* What main.cpp's first-counter handler (on_mesh_first_counter) does, on a
 * member's first verified frame and on the second save 66 s later: its
 * persist_replay_counters(), into a stand-in for the NVS replay_ctrs blob,
 * returning whether the save landed, and the boot's restore loop over that
 * blob. g_first_counter_fails makes the next that-many calls fail as
 * main.cpp's does when NVS refuses the write (Preferences::begin or
 * putBytes): nothing saved, false returned. SIZE_MAX fails every call, as
 * main.cpp's does on a board without flash encryption. */
struct SavedCounters {
  size_t   n = 0;
  uint8_t  fps[mesh_session::MAX_REPLAY_COUNTERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs[mesh_session::MAX_REPLAY_COUNTERS];
};
SavedCounters g_nvs_counters;
size_t        g_first_counters = 0;
size_t        g_first_counter_fails = 0;
uint8_t       g_first_counter_fp[mesh_crypto::FINGERPRINT_LEN];
bool persist_counters_on_first(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) {
  ++g_first_counters;
  std::memcpy(g_first_counter_fp, fp, sizeof(g_first_counter_fp));
  if (g_first_counter_fails > 0) {
    if (g_first_counter_fails != SIZE_MAX) --g_first_counter_fails;
    return false;
  }
  g_nvs_counters.n = mesh_session::get_replay_counters(g_nvs_counters.fps, g_nvs_counters.ctrs,
                                                       mesh_session::MAX_REPLAY_COUNTERS);
  return true;
}
void restore_counters_at_boot() {
  for (size_t k = 0; k < g_nvs_counters.n; ++k) {
    mesh_session::restore_replay_counter(g_nvs_counters.fps[k], g_nvs_counters.ctrs[k]);
  }
}

/* A member's first counter is saved at once (F324). A receiver pairs with a
 * member that has been in the opera an hour, heartbeating its other members
 * every interval under counters this receiver never held. The member's
 * first frame here, its paired heartbeat, fires the first-counter handler
 * once, and main.cpp persists the counters then. So a receiver rebooted
 * before its first 5-minute save restores that counter, and the hour's
 * frames, recorded and relayed from the member's own address after the
 * member is gone, refresh nothing: it stays OFFLINE. Fails without the
 * save: the receiver restored no counter for the member, and the same relay
 * held it CONNECTED about 90 s per frame (probed before the handler: 240
 * frames, 356 minutes). The handler fires for nothing else: not the
 * member's next frame, a replay, junk or a forgery, and not a first frame
 * after a boot that restored the member's counter, so a boot costs no
 * write. A boot that restored none (no save had caught it) fires it again,
 * and so does the first frame after a re-pair from the member's replay
 * tombstone, whose counter is not 0. So does the first frame after a boot
 * that restored a saved 0: a save that ran while the member was registered
 * and not yet heard wrote it at 0, and counters start at 1, so that entry is
 * no counter (review round 3: a restore that marked a saved 0 saved fired
 * nothing, and the frames the member signed before held it CONNECTED after
 * the next reboot). That this first frame need not be the member's newest
 * is test_an_older_first_frame_is_saved_over_once_more's. */
void test_a_members_first_counter_is_saved_at_once() {
  const uint32_t step = mesh_transport::PEER_STALE_AFTER_MS - 1000;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x2B + i);
  uint8_t pub[32], priv[32];
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0xD0, 0x01};
  auto heartbeat = [&](TestMember& m) {
    std::vector<uint8_t> f(mesh_envelope::MAX_FRAME_LEN);
    f.resize(build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                        mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, f.data(),
                                        f.size()));
    assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
    return f;
  };
  auto deliver = [&](const std::vector<uint8_t>& f, uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::test::inject_recv(mac, f.data(), f.size(), -60);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto boot = [&]() {   /* main.cpp setup(): the session, then the handlers */
    stand_up_session(S, pub, priv);
    mesh_session::set_first_counter_handler(persist_counters_on_first);
  };
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember m;
  make_member(m, mac);   /* the pairing registers and binds it: no counter held */
  assert(g_first_counters == 0);
  /* Its hour in the opera before this pairing, to its other members. */
  std::vector<std::vector<uint8_t>> hour;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) hour.push_back(heartbeat(m));
  /* The paired heartbeat: the first frame this receiver hears from it. */
  uint32_t t = 1000;
  const std::vector<uint8_t> paired = heartbeat(m);
  deliver(paired, t);
  assert(has(peer_row(t, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  assert(g_first_counters == 1 && std::memcmp(g_first_counter_fp, m.fp, sizeof(m.fp)) == 0);
  assert(g_nvs_counters.n == 1 && std::memcmp(g_nvs_counters.fps[0], m.fp, sizeof(m.fp)) == 0 &&
         g_nvs_counters.ctrs[0] == m.counter);
  /* Nothing else fires it: the next frame, a replay, junk, a forgery. */
  t += kBeatGapMin;
  deliver(heartbeat(m), t);
  deliver(paired, t + 1000);
  const std::vector<uint8_t> junk = {mesh_envelope::OPERA_VERSION, 0xEE, 0xEE};
  deliver(junk, t + 2000);
  std::vector<uint8_t> forged = heartbeat(m);
  forged.back() ^= 0x01;
  deliver(forged, t + 3000);
  assert(g_first_counters == 1);
  /* The member is gone, and the receiver reboots before the next save. */
  boot();
  assert(mesh_session::register_trusted_peer(m.pub));
  assert(mesh_session::bind_peer_mac(m.fp, mac));
  restore_counters_at_boot();
  const uint32_t r0 = t + mesh_transport::PEER_OFFLINE_AFTER_MS;
  for (size_t k = 0; k < hour.size(); ++k) {
    const uint32_t at = r0 + (uint32_t)k * step;
    deliver(hour[k], at);   /* below the saved counter: a replay */
    assert(has(peer_row(at, m.pub), "\"state\":\"OFFLINE\","));
    assert(has(status_body(), "\"state\":\"CONNECTING\"") &&
           has(status_body(), "\"peers_online\":0,"));
  }
  deliver(paired, r0 + (uint32_t)hour.size() * step);   /* the saved one, too */
  assert(has(peer_row(r0 + (uint32_t)hour.size() * step, m.pub), "\"state\":\"OFFLINE\","));
  assert(g_first_counters == 1);
  /* The member itself, back: CONNECTED at once, and a restored counter
   * fires nothing. */
  t = r0 + (uint32_t)(hour.size() + 1) * step;
  deliver(heartbeat(m), t);
  assert(has(peer_row(t, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  assert(g_first_counters == 1);
  /* A boot that restored no counter for it: the first frame saves one. */
  g_nvs_counters = SavedCounters{};
  boot();
  assert(mesh_session::register_trusted_peer(m.pub));
  assert(mesh_session::bind_peer_mac(m.fp, mac));
  restore_counters_at_boot();
  t += 1000;
  deliver(heartbeat(m), t);
  assert(g_first_counters == 2 && g_nvs_counters.n == 1 && g_nvs_counters.ctrs[0] == m.counter);
  /* A member dropped and re-paired resumes from its replay tombstone, whose
   * counter is not 0; its first frame after the re-pair still saves (the
   * frames it signed meanwhile are past the tombstone). */
  assert(mesh_session::unregister_trusted_peer(m.fp));
  for (int k = 0; k < 3; ++k) (void)heartbeat(m);   /* to its other members */
  assert(mesh_session::register_trusted_peer(m.pub));
  assert(mesh_session::bind_peer_mac(m.fp, mac));
  t += 1000;
  deliver(heartbeat(m), t);
  assert(has(peer_row(t, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  assert(g_first_counters == 3);
  bool saved_now = false;
  for (size_t k = 0; k < g_nvs_counters.n; ++k) {
    saved_now |= std::memcmp(g_nvs_counters.fps[k], m.fp, sizeof(m.fp)) == 0 &&
                 g_nvs_counters.ctrs[k] == m.counter;
  }
  assert(saved_now);
  /* A save while a member is registered and not yet heard (another
   * member's first counter, a leave, the 5-minute save) writes it at 0. A
   * boot that restores that 0 restored no counter: the member's first frame
   * after it is saved, and after one more reboot the frames it signed before
   * stay replays. */
  boot();
  TestMember z;
  make_member(z, mac);
  std::vector<std::vector<uint8_t>> before;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) before.push_back(heartbeat(z));
  g_nvs_counters.n = mesh_session::get_replay_counters(g_nvs_counters.fps, g_nvs_counters.ctrs,
                                                       mesh_session::MAX_REPLAY_COUNTERS);
  assert(g_nvs_counters.n == 1 && g_nvs_counters.ctrs[0] == 0);
  boot();
  assert(mesh_session::register_trusted_peer(z.pub));
  assert(mesh_session::bind_peer_mac(z.fp, mac));
  restore_counters_at_boot();
  const size_t fired_before_zero = g_first_counters;
  t += 1000;
  deliver(heartbeat(z), t);
  assert(has(peer_row(t, z.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  assert(g_first_counters == fired_before_zero + 1);
  assert(g_nvs_counters.n == 1 && std::memcmp(g_nvs_counters.fps[0], z.fp, sizeof(z.fp)) == 0 &&
         g_nvs_counters.ctrs[0] == z.counter);
  boot();
  assert(mesh_session::register_trusted_peer(z.pub));
  assert(mesh_session::bind_peer_mac(z.fp, mac));
  restore_counters_at_boot();
  const uint32_t z0 = t + mesh_transport::PEER_OFFLINE_AFTER_MS;
  for (size_t k = 0; k < before.size(); ++k) {
    const uint32_t at = z0 + (uint32_t)k * step;
    deliver(before[k], at);   /* below the saved counter: a replay */
    assert(has(peer_row(at, z.pub), "\"state\":\"OFFLINE\","));
  }
  std::printf("PASS test_a_members_first_counter_is_saved_at_once"
              "  (%zu recorded frames after a reboot before the save: none refreshed it;"
              " a saved 0 restored no counter, %zu more)\n",
              hour.size(), before.size());
}

/* The first-counter save's second half (F324, review round 3). The first
 * frame that verifies from a newly registered member is saved at once, but
 * it need not be the member's newest: the counter check is a high-water
 * mark, so any older genuine frame of the member's passes too, and a radio
 * copying the member's address can land one before the member's own first
 * frame. The window for that runs until the member's own first frame lands:
 * after a pairing, milliseconds normally, up to about one heartbeat
 * interval when the first COMPLETE or the paired heartbeat is lost
 * (test_a_lost_complete_is_healed_by_the_next_heartbeat), which a jammer
 * can force; after a boot that restored no counter for the member, until
 * the member's next heartbeat is heard, later still while this device's
 * station has not joined its router (F320). The first save then holds
 * that older counter, and the member's own frames raise it in RAM only.
 * FIRST_COUNTER_RESAVE_MS later the handler runs once more, the counter
 * having moved, and the save holds the member's own: a receiver rebooted
 * after that restores it, and the older frames, relayed after the member
 * is gone, stay replays. Fails without the second save (review round 3's
 * probe: the save held counter 1, and 119 relayed frames held the gone
 * member CONNECTED at 119 of 119 reads).
 * Pinned beside it: a reboot BEFORE the second save restores the older
 * counter, and the frames after it hold the gone member CONNECTED, and a
 * reboot before the member's first frame lands restores no counter at all
 * (the residuals the docs state); the second save is taken exactly
 * FIRST_COUNTER_RESAVE_MS after the first, not after the member's later
 * frames, and not 1 ms earlier (a deadline armed up to 999 ms early passed
 * every leg, whose passes ran on a 1 s grid, until the third review of the
 * Codex fix); it outlasts one lost heartbeat of the member's own, timed from
 * the heartbeat constants (review round 4: a deadline of 33 s or 45 s found
 * nothing moved and kept counter 1), at t0 = 1000 and across the millis()
 * wrap (a deadline taken as `now < from + R` fired at once there); it runs
 * once; it writes nothing when the counter has not moved, and disarms then
 * (a frame after the deadline adds no save); it runs while the session is
 * stopped; and a member dropped before it gets none. */
void test_an_older_first_frame_is_saved_over_once_more() {
  const uint32_t pass_ms = 1000;
  const uint32_t step = mesh_transport::PEER_STALE_AFTER_MS - pass_ms;
  constexpr uint32_t R = mesh_session::FIRST_COUNTER_RESAVE_MS;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x3C + i);
  uint8_t pub[32], priv[32];
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0xD1, 0x01};
  auto heartbeat = [&](TestMember& m) {
    std::vector<uint8_t> f(mesh_envelope::MAX_FRAME_LEN);
    f.resize(build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                        mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, f.data(),
                                        f.size()));
    assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
    return f;
  };
  auto deliver = [&](const std::vector<uint8_t>& f, uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::test::inject_recv(mac, f.data(), f.size(), -60);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto idle = [&](uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto boot = [&]() {
    stand_up_session(S, pub, priv);
    mesh_session::set_first_counter_handler(persist_counters_on_first);
  };
  auto saved_counter = [&](const TestMember& m) {
    for (size_t k = 0; k < g_nvs_counters.n; ++k) {
      if (std::memcmp(g_nvs_counters.fps[k], m.fp, sizeof(m.fp)) == 0) return g_nvs_counters.ctrs[k];
    }
    return (uint64_t)UINT64_MAX;
  };
  auto reboot_restoring = [&](const TestMember& m) {   /* main.cpp's boot restore */
    boot();
    assert(mesh_session::register_trusted_peer(m.pub));
    assert(mesh_session::bind_peer_mac(m.fp, mac));
    restore_counters_at_boot();
  };
  /* The member's frames to its other members before this pairing, and the
   * receiver's reads after a reboot as a recorder relays all but the
   * first, one every `step`, with the member gone. */
  auto relayed_connected = [&](const TestMember& m, const std::vector<std::vector<uint8_t>>& rec,
                               uint32_t r0) {
    size_t connected = 0;
    for (size_t k = 1; k < rec.size(); ++k) {
      const uint32_t at = r0 + (uint32_t)(k - 1) * step;
      deliver(rec[k], at);
      if (has(peer_row(at, m.pub), "\"state\":\"CONNECTED\",")) ++connected;
    }
    return connected;
  };
  const uint32_t t0 = 1000;

  /* The relayed older frame lands first; the member's own paired heartbeat
   * half a second later. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember m;
  make_member(m, mac);
  std::vector<std::vector<uint8_t>> rec;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) rec.push_back(heartbeat(m));
  deliver(rec[0], t0);
  assert(g_first_counters == 1 && saved_counter(m) == 1);
  deliver(heartbeat(m), t0 + 500);
  const uint64_t own = m.counter;
  assert(g_first_counters == 1 && saved_counter(m) == 1);   /* the older counter, saved */
  for (uint32_t t = t0 + pass_ms; t < t0 + R; t += pass_ms) idle(t);
  assert(g_first_counters == 1);
  idle(t0 + R - 1);   /* to the millisecond: not earlier (a deadline armed up
                       * to 999 ms early passed every 1 s pass) */
  assert(g_first_counters == 1);
  idle(t0 + R);   /* exactly R after the first save, not after the later frame */
  assert(g_first_counters == 2 && saved_counter(m) == own);
  /* Once: the member's later frames add no save. */
  deliver(heartbeat(m), t0 + R + kBeatGapMin);
  for (uint32_t t = t0 + R + kBeatGapMin + pass_ms; t <= t0 + 3 * R; t += pass_ms) idle(t);
  assert(g_first_counters == 2);
  /* The member is gone; a reboot before the 5-minute save restores its own
   * counter, and the relayed older frames refresh nothing. */
  reboot_restoring(m);
  const size_t after_resave = relayed_connected(m, rec, t0 + 3 * R + mesh_transport::PEER_OFFLINE_AFTER_MS);
  assert(after_resave == 0);
  assert(has(status_body(), "\"state\":\"CONNECTING\"") &&
         has(status_body(), "\"peers_online\":0,"));

  /* The residual: a reboot before the second save restores the older
   * counter, and every frame after it verifies once more, the member gone. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember e;
  make_member(e, mac);
  std::vector<std::vector<uint8_t>> rec_e;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) rec_e.push_back(heartbeat(e));
  deliver(rec_e[0], t0);
  deliver(heartbeat(e), t0 + 500);
  idle(t0 + R - pass_ms);
  assert(g_first_counters == 1 && saved_counter(e) == 1);
  reboot_restoring(e);
  const size_t before_resave = relayed_connected(e, rec_e, t0 + R + mesh_transport::PEER_OFFLINE_AFTER_MS);
  assert(before_resave == rec_e.size() - 1);

  /* The other residual: a reboot before the member's first frame lands.
   * The paired heartbeat is lost and the receiver is unplugged 20 s after
   * the bind: nothing was saved for the member, the boot restores no
   * counter for it, and every frame it signed before the pairing verifies
   * once more, the member gone. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember b;
  make_member(b, mac);
  std::vector<std::vector<uint8_t>> rec_b;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) rec_b.push_back(heartbeat(b));
  (void)heartbeat(b);   /* the paired heartbeat, lost */
  for (uint32_t t = t0; t <= t0 + 20000; t += pass_ms) idle(t);
  assert(g_first_counters == 0 && g_nvs_counters.n == 0);
  reboot_restoring(b);
  const size_t before_first =
      relayed_connected(b, rec_b, t0 + 20000 + mesh_transport::PEER_OFFLINE_AFTER_MS);
  assert(before_first == rec_b.size() - 1);

  /* One lost heartbeat of the member's own (review round 4). The older frame
   * lands first, the member's next heartbeat is lost, and the one after it
   * lands two of the longest heartbeat intervals less one pass after the
   * older frame: times from the heartbeat constants, not from R, so a
   * deadline too short to outlast the one loss fails here (33 s, 45 s and
   * 64 s found nothing moved and kept counter 1 saved; review round 4's
   * probe then read the gone member CONNECTED at 119 of 119 reads after a
   * reboot). The second save holds the member's own counter, and a reboot
   * right after it leaves every relayed older frame a replay. Run twice: at
   * t0 = 1000, and with the first save 30 s before the millis() wrap, the
   * transport and session clocks wrapping between it and the deadline,
   * where a deadline taken as `now < from + R` wraps to a small number and
   * fires at once, before the member's frame (it fails here the same way). */
  constexpr uint32_t kLongestGap =
      mesh_session::HEARTBEAT_INTERVAL_MS + mesh_session::HEARTBEAT_JITTER_MS;
  auto one_lost_own_heartbeat = [&](uint32_t s0) {
    g_nvs_counters = SavedCounters{};
    g_first_counters = 0;
    boot();
    TestMember l;
    make_member(l, mac);
    std::vector<std::vector<uint8_t>> rec_l;
    for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) rec_l.push_back(heartbeat(l));
    const uint32_t lost_dt = kLongestGap - pass_ms;
    const uint32_t own_dt  = 2 * kLongestGap - pass_ms;
    deliver(rec_l[0], s0);
    assert(g_first_counters == 1 && saved_counter(l) == 1);
    for (uint32_t dt = pass_ms; dt < lost_dt; dt += pass_ms) idle(s0 + dt);
    (void)heartbeat(l);   /* sent at s0 + lost_dt, lost */
    for (uint32_t dt = lost_dt; dt < own_dt; dt += pass_ms) idle(s0 + dt);
    assert(g_first_counters == 1 && saved_counter(l) == 1);
    deliver(heartbeat(l), s0 + own_dt);
    const uint64_t own_l = l.counter;
    for (uint32_t dt = own_dt + pass_ms; dt <= R; dt += pass_ms) idle(s0 + dt);
    assert(g_first_counters == 2 && saved_counter(l) == own_l);
    reboot_restoring(l);
    const size_t connected =
        relayed_connected(l, rec_l, s0 + R + mesh_transport::PEER_OFFLINE_AFTER_MS);
    assert(connected == 0);
    return rec_l.size() - 1;
  };
  const size_t lost_relayed = one_lost_own_heartbeat(t0);
  const size_t wrap_relayed = one_lost_own_heartbeat(0u - 30000u);
  assert(lost_relayed == wrap_relayed);

  /* Nothing moved: no second write. The member's own first frame, then
   * silence. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember q;
  make_member(q, mac);
  deliver(heartbeat(q), t0);
  assert(g_first_counters == 1 && saved_counter(q) == q.counter);
  for (uint32_t t = t0 + pass_ms; t <= t0 + 2 * R; t += pass_ms) idle(t);
  assert(g_first_counters == 1);
  /* And it disarmed at the deadline: the member's next frame, after it,
   * moves the counter and adds no save (a resave left armed until the
   * counter moved wrote here). */
  deliver(heartbeat(q), t0 + 2 * R + pass_ms);
  for (uint32_t t = t0 + 2 * R + 2 * pass_ms; t <= t0 + 3 * R; t += pass_ms) idle(t);
  assert(g_first_counters == 1);

  /* Stopped: the mesh turned off after the member's next frame, and the
   * second save still lands at its deadline. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember d;
  make_member(d, mac);
  deliver(heartbeat(d), t0);
  deliver(heartbeat(d), t0 + kBeatGapMin);
  mesh_session::set_enabled(false);
  for (uint32_t t = t0 + kBeatGapMin + pass_ms; t < t0 + R; t += pass_ms) idle(t);
  assert(g_first_counters == 1);
  idle(t0 + R);
  assert(g_first_counters == 2 && saved_counter(d) == d.counter);

  /* Dropped before its deadline: nothing more for it. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  boot();
  TestMember g;
  make_member(g, mac);
  deliver(heartbeat(g), t0);
  deliver(heartbeat(g), t0 + kBeatGapMin);
  assert(g_first_counters == 1);
  assert(mesh_session::unregister_trusted_peer(g.fp));
  for (uint32_t t = t0 + kBeatGapMin + pass_ms; t <= t0 + 2 * R; t += pass_ms) idle(t);
  assert(g_first_counters == 1);

  std::printf("PASS test_an_older_first_frame_is_saved_over_once_more"
              "  (saved over %u ms after the first save: %zu relayed frames, none refreshed it,"
              " nor %zu after one lost own heartbeat, at t0 and across the wrap;"
              " a reboot before it: %zu of %zu; before the first frame: %zu of %zu)\n",
              (unsigned)R, rec.size() - 1, lost_relayed, before_resave, rec_e.size() - 1,
              before_first, rec_b.size() - 1);
}

/* A failed first-counter save is tried again (F324; the Codex review of
 * #1839, P2). The session cleared a member's `counter_unsaved` and armed the
 * second save before it ran a handler that returned nothing, and main.cpp
 * dropped save_replay_counters()'s result, so an NVS write that failed once
 * (Preferences::begin or putBytes) was taken as done: the member's next
 * frames saved nothing, and the receiver held no durable counter for it
 * until the 66 s second save, or, when the counter had not moved by then,
 * until the next 5-minute save. A reboot in between restored no counter,
 * and every frame the member had signed under the opera before the pairing
 * verified again. The handler now returns whether the save landed; on false
 * the counter stays unsaved, and the member's next frame that passes every
 * check runs it again. Pinned here: one refused write is retried on the
 * member's next verified frame, not on a pass of its own, and not on a
 * replay, junk or a forgery (the old code ran it once: this fails on the
 * second frame); a reboot after that retry, before either 66 s deadline,
 * restores the member's counter, and the frames it signed before the
 * pairing stay replays, while a reboot before that retry lands restores
 * none and they read CONNECTED again (the residual the docs state, pinned,
 * not fixed), for as long as the member stays quiet: no pass retries it, so
 * it waits for the member's next verified frame or a 5-minute save that
 * lands first (pinned too); the second save is armed from the save that
 * landed, not from the call that failed, to the millisecond; a frame before
 * any handler is installed leaves the counter unsaved too; and a handler
 * that fails on every call (main.cpp's on a board without flash
 * encryption, which writes no replay counter at all) runs once per verified
 * frame of the member and on no other pass, and never arms the second
 * save. */
void test_a_failed_first_counter_save_is_tried_again() {
  const uint32_t pass_ms = 1000;
  const uint32_t step = mesh_transport::PEER_STALE_AFTER_MS - pass_ms;
  constexpr uint32_t R = mesh_session::FIRST_COUNTER_RESAVE_MS;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x4D + i);
  uint8_t pub[32], priv[32];
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0xD2, 0x01};
  auto heartbeat = [&](TestMember& m) {
    std::vector<uint8_t> f(mesh_envelope::MAX_FRAME_LEN);
    f.resize(build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                        mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, f.data(),
                                        f.size()));
    assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
    return f;
  };
  auto deliver = [&](const std::vector<uint8_t>& f, uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::test::inject_recv(mac, f.data(), f.size(), -60);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto idle = [&](uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto boot = [&]() {
    stand_up_session(S, pub, priv);
    mesh_session::set_first_counter_handler(persist_counters_on_first);
  };
  auto saved_counter = [&](const TestMember& m) {
    for (size_t k = 0; k < g_nvs_counters.n; ++k) {
      if (std::memcmp(g_nvs_counters.fps[k], m.fp, sizeof(m.fp)) == 0) return g_nvs_counters.ctrs[k];
    }
    return (uint64_t)UINT64_MAX;
  };
  auto reboot_restoring = [&](const TestMember& m) {   /* main.cpp's boot restore */
    boot();
    assert(mesh_session::register_trusted_peer(m.pub));
    assert(mesh_session::bind_peer_mac(m.fp, mac));
    restore_counters_at_boot();
  };
  const std::vector<uint8_t> junk = {mesh_envelope::OPERA_VERSION, 0xEE, 0xEE};
  const uint32_t t0 = 1000;
  const uint32_t t1 = t0 + kBeatGapMin;   /* the member's next heartbeat, at its soonest */

  /* One write refused, then the member's next heartbeat: saved then. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  g_first_counter_fails = 1;
  boot();
  TestMember m;
  make_member(m, mac);
  std::vector<std::vector<uint8_t>> rec;   /* its hour before the pairing */
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) rec.push_back(heartbeat(m));
  const std::vector<uint8_t> paired = heartbeat(m);
  deliver(paired, t0);
  assert(has(peer_row(t0, m.pub), "\"state\":\"CONNECTED\",\"last_seen_sec\":0,"));
  assert(g_first_counters == 1 && g_nvs_counters.n == 0);   /* refused: nothing saved */
  /* Nothing retries on a pass of its own, nor on a replay, junk or a forgery. */
  std::vector<uint8_t> forged = heartbeat(m);
  forged.back() ^= 0x01;
  --m.counter;   /* never accepted: the member's next frame reuses its counter */
  for (uint32_t t = t0 + pass_ms; t < t1; t += pass_ms) {
    if (t == t0 + 5 * pass_ms)      deliver(paired, t);
    else if (t == t0 + 6 * pass_ms) deliver(junk, t);
    else if (t == t0 + 7 * pass_ms) deliver(forged, t);
    else                            idle(t);
  }
  assert(g_first_counters == 1 && g_nvs_counters.n == 0);
  deliver(heartbeat(m), t1);
  assert(g_first_counters == 2 && std::memcmp(g_first_counter_fp, m.fp, sizeof(m.fp)) == 0);
  assert(saved_counter(m) == m.counter);
  /* A reboot after that retry, inside the first 66 s: the member's counter
   * is restored, and the frames it signed before the pairing, relayed from
   * its address with the member gone, refresh nothing. */
  idle(t1 + pass_ms);
  assert(t1 + pass_ms < t0 + R);
  reboot_restoring(m);
  const uint32_t r0 = t1 + mesh_transport::PEER_OFFLINE_AFTER_MS;
  size_t relayed_connected = 0;
  for (size_t k = 0; k < rec.size(); ++k) {
    const uint32_t at = r0 + (uint32_t)k * step;
    deliver(rec[k], at);
    if (has(peer_row(at, m.pub), "\"state\":\"CONNECTED\",")) ++relayed_connected;
  }
  assert(relayed_connected == 0);

  /* What stays open, and the docs say so: a reboot after the refused first
   * save and before its retry on the member's next frame lands. Nothing is
   * saved for the member yet, so the boot restores no counter for it, and
   * the frames it signed before the pairing, relayed from its address with
   * the member gone, hold it CONNECTED again (the review of the Codex fix:
   * the roadmap and LESSONS_LEARNED.md named only a reboot before the
   * first frame lands). */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  g_first_counter_fails = 1;
  boot();
  TestMember e;
  make_member(e, mac);
  std::vector<std::vector<uint8_t>> e_rec;
  for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) e_rec.push_back(heartbeat(e));
  deliver(heartbeat(e), t0);
  assert(g_first_counters == 1 && g_nvs_counters.n == 0);   /* landed; its save refused */
  idle(t0 + 10 * pass_ms);
  assert(t0 + 10 * pass_ms < t1);   /* before the member's next heartbeat */
  reboot_restoring(e);
  assert(g_nvs_counters.n == 0);
  const uint32_t e0 = t0 + 10 * pass_ms + mesh_transport::PEER_OFFLINE_AFTER_MS;
  size_t reopened = 0;
  for (size_t k = 0; k < e_rec.size(); ++k) {
    const uint32_t at = e0 + (uint32_t)k * step;
    deliver(e_rec[k], at);
    if (has(peer_row(at, e.pub), "\"state\":\"CONNECTED\",")) ++reopened;
  }
  assert(reopened == e_rec.size());

  /* And how long it stays open when the member goes quiet: its own frames
   * kept from the receiver after the refused save (lost, or a jammer). No
   * pass retries the first save, only the member's next verified frame
   * does, so the save waits for that frame or for main.cpp's 5-minute save
   * (its loop calls persist_replay_counters() every 300 s, a snapshot of
   * every live counter, this member's too) when that one lands first (the
   * third review of the Codex fix: the docs bounded this window by the
   * member's next heartbeat alone). Both pinned: 290 s of passes with the
   * member quiet run the handler on none of them, and a reboot then reopens
   * every frame the member signed before the pairing; the same 290 s with a
   * 5-minute save landed at its end leave them replays. */
  constexpr uint32_t kQuietMs = 290000;
  size_t quiet_frames = 0;
  auto quiet_after_refusal = [&](bool five_minute_save) {
    g_nvs_counters = SavedCounters{};
    g_first_counters = 0;
    g_first_counter_fails = 1;
    boot();
    TestMember j;
    make_member(j, mac);
    std::vector<std::vector<uint8_t>> j_rec;
    for (uint32_t k = 0; k < 3600000 / kBeatGapMin; ++k) j_rec.push_back(heartbeat(j));
    deliver(heartbeat(j), t0);
    assert(g_first_counters == 1 && g_nvs_counters.n == 0);   /* refused */
    for (uint32_t t = t0 + pass_ms; t <= t0 + kQuietMs; t += pass_ms) idle(t);
    assert(g_first_counters == 1 && g_nvs_counters.n == 0);   /* no pass retried it */
    if (five_minute_save) {   /* main.cpp's loop: persist_replay_counters() */
      g_nvs_counters.n = mesh_session::get_replay_counters(g_nvs_counters.fps, g_nvs_counters.ctrs,
                                                           mesh_session::MAX_REPLAY_COUNTERS);
      assert(saved_counter(j) == j.counter);
    }
    reboot_restoring(j);
    const uint32_t j0 = t0 + kQuietMs + mesh_transport::PEER_OFFLINE_AFTER_MS;
    size_t connected = 0;
    for (size_t k = 0; k < j_rec.size(); ++k) {
      const uint32_t at = j0 + (uint32_t)k * step;
      deliver(j_rec[k], at);
      if (has(peer_row(at, j.pub), "\"state\":\"CONNECTED\",")) ++connected;
    }
    assert(g_first_counter_fails == 0);
    quiet_frames = j_rec.size();
    return connected;
  };
  const size_t quiet_open = quiet_after_refusal(false);
  assert(quiet_open == quiet_frames);
  assert(quiet_after_refusal(true) == 0);

  /* The second save is armed from the save that landed: R after t1, not
   * after the call at t0 that failed. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  g_first_counter_fails = 1;
  boot();
  TestMember a;
  make_member(a, mac);
  deliver(heartbeat(a), t0);
  assert(g_first_counters == 1 && g_nvs_counters.n == 0);
  deliver(heartbeat(a), t1);
  const uint64_t landed = a.counter;
  assert(g_first_counters == 2 && saved_counter(a) == landed);
  for (uint32_t t = t1 + pass_ms; t < t1 + R; t += pass_ms) {
    if (t == t1 + kBeatGapMin) deliver(heartbeat(a), t);   /* the counter moves */
    else                       idle(t);
  }
  assert(g_first_counters == 2 && saved_counter(a) == landed);   /* t0 + R armed nothing */
  idle(t1 + R - 1);   /* to the millisecond */
  assert(g_first_counters == 2 && saved_counter(a) == landed);
  idle(t1 + R);
  assert(g_first_counters == 3 && saved_counter(a) == a.counter && a.counter > landed);

  /* A frame before any handler is installed leaves the counter unsaved. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  stand_up_session(S, pub, priv);   /* no handler */
  TestMember h;
  make_member(h, mac);
  deliver(heartbeat(h), t0);
  mesh_session::set_first_counter_handler(persist_counters_on_first);
  for (uint32_t t = t0 + pass_ms; t < t1; t += pass_ms) idle(t);
  assert(g_first_counters == 0);
  deliver(heartbeat(h), t1);
  assert(g_first_counters == 1 && saved_counter(h) == h.counter);

  /* Every call fails (a board without flash encryption): once per verified
   * frame of the member, on no other pass, and no second save, over ten
   * minutes of heartbeats with a replay and junk between them. */
  g_nvs_counters = SavedCounters{};
  g_first_counters = 0;
  g_first_counter_fails = SIZE_MAX;
  boot();
  TestMember f;
  make_member(f, mac);
  size_t verified = 0;
  std::vector<uint8_t> last;
  for (uint32_t dt = 0; dt <= 600000; dt += pass_ms) {
    const uint32_t t = t0 + dt;
    const uint32_t phase = dt % kBeatGapMin;
    if (phase == 0) {
      last = heartbeat(f);
      deliver(last, t);
      ++verified;
    } else if (phase == 5 * pass_ms) {
      deliver(last, t);   /* a replay */
    } else if (phase == 6 * pass_ms) {
      deliver(junk, t);
    } else {
      idle(t);
    }
    assert(g_first_counters == verified);
  }
  assert(verified == 600000 / kBeatGapMin + 1 && g_nvs_counters.n == 0);
  assert(has(peer_row(t0 + 600000, f.pub), "\"state\":\"CONNECTED\","));   /* liveness unaffected */
  for (uint32_t t = t0 + 600000 + pass_ms; t <= t0 + 600000 + 3 * R; t += pass_ms) idle(t);
  assert(g_first_counters == verified && g_nvs_counters.n == 0);
  g_first_counter_fails = 0;

  std::printf("PASS test_a_failed_first_counter_save_is_tried_again"
              "  (a refused write saved on the member's next frame, %u ms later: %zu relayed"
              " frames after a reboot, none refreshed it; a reboot before that retry: %zu of"
              " %zu read CONNECTED, and %zu of %zu after %u s with the member quiet, none once a"
              " 5-minute save landed; every call refused: %zu calls for %zu verified frames)\n",
              (unsigned)(t1 - t0), rec.size(), reopened, e_rec.size(), quiet_open, quiet_frames,
              (unsigned)(kQuietMs / 1000), g_first_counters, verified);
}

/* A failed second save is tried again a full FIRST_COUNTER_RESAVE_MS later
 * (F324; the Codex review of #1839). resave_first_counters() disarmed
 * before it ran the handler and dropped its result, so a write refused at
 * the 66 s deadline was never retried by the session, and the member's own
 * counter waited for the 5-minute save while the saved one was the older
 * frame's. The handler's false now moves the deadline to that pass, so the
 * save is taken again R later, and then disarms once it lands; a pass
 * that comes late, two deadlines after the member's heartbeat, runs it
 * once and takes the retry R after itself, not after the deadline it
 * missed (none on the passes in between, nor 1 ms before it); and a
 * handler that fails on every call from the deadline on runs exactly once
 * per R (no call on the passes in between, none on the member's frames,
 * whose first save landed), at t0 = 1000 and with the clock wrapping
 * between the first save and the retries. Fails on the old code: no call
 * after the deadline's; the late-pass leg also on a retry deadline moved
 * on by R from the one it missed (`from_ms += R`), which then runs on
 * three passes in a row, and on one set up to 999 ms early (`= now - 1`),
 * which every 1 s pass let through until the second review of the Codex
 * fix. */
void test_a_failed_second_save_is_tried_again_a_deadline_later() {
  const uint32_t pass_ms = 1000;
  constexpr uint32_t R = mesh_session::FIRST_COUNTER_RESAVE_MS;
  uint8_t S[32];
  for (size_t i = 0; i < sizeof(S); ++i) S[i] = (uint8_t)(0x5E + i);
  uint8_t pub[32], priv[32];
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x32, 0xD3, 0x01};
  auto heartbeat = [&](TestMember& m) {
    std::vector<uint8_t> f(mesh_envelope::MAX_FRAME_LEN);
    f.resize(build_signed_session_frame(m.pub, m.priv, S, ++m.counter,
                                        mesh_envelope::MsgType::HEARTBEAT, nullptr, 0, f.data(),
                                        f.size()));
    assert(f.size() == mesh_envelope::MIN_FRAME_LEN);
    return f;
  };
  auto deliver = [&](const std::vector<uint8_t>& f, uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::test::inject_recv(mac, f.data(), f.size(), -60);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto idle = [&](uint32_t t) {
    mesh_transport::test::set_now_ms(t);
    mesh_transport::process();
    mesh_session::process(t);
  };
  auto boot = [&]() {
    stand_up_session(S, pub, priv);
    mesh_session::set_first_counter_handler(persist_counters_on_first);
  };
  auto saved_counter = [&](const TestMember& m) {
    for (size_t k = 0; k < g_nvs_counters.n; ++k) {
      if (std::memcmp(g_nvs_counters.fps[k], m.fp, sizeof(m.fp)) == 0) return g_nvs_counters.ctrs[k];
    }
    return (uint64_t)UINT64_MAX;
  };
  size_t persistent_calls = 0;
  auto run = [&](uint32_t s0) {
    /* One refused write at the deadline, then the retry R later. */
    g_nvs_counters = SavedCounters{};
    g_first_counters = 0;
    g_first_counter_fails = 0;
    boot();
    TestMember q;
    make_member(q, mac);
    deliver(heartbeat(q), s0);
    const uint64_t first = q.counter;
    assert(g_first_counters == 1 && saved_counter(q) == first);
    deliver(heartbeat(q), s0 + kBeatGapMin);
    const uint64_t own = q.counter;
    g_first_counter_fails = 1;
    for (uint32_t dt = kBeatGapMin + pass_ms; dt < R; dt += pass_ms) idle(s0 + dt);
    assert(g_first_counters == 1);
    idle(s0 + R);
    assert(g_first_counters == 2 && saved_counter(q) == first);   /* refused */
    for (uint32_t dt = R + pass_ms; dt < 2 * R; dt += pass_ms) idle(s0 + dt);
    assert(g_first_counters == 2);   /* no call on the passes in between */
    idle(s0 + 2 * R);
    assert(g_first_counters == 3 && saved_counter(q) == own);
    /* Landed: disarmed, and the member's later frames add no save. */
    for (uint32_t dt = 2 * R + pass_ms; dt <= 5 * R; dt += pass_ms) {
      if (dt % kBeatGapMin == 0) deliver(heartbeat(q), s0 + dt);
      else                       idle(s0 + dt);
    }
    assert(g_first_counters == 3 && saved_counter(q) == own);

    /* A late pass: the main loop ran no pass from the member's heartbeat
     * until two deadlines had gone by. That pass runs the handler once,
     * and the write it refused is tried again R after that pass, not R
     * after the deadline it missed: a retry counted from the missed
     * deadline would run on each of the passes after it until it caught
     * up, one NVS write a pass (the review of the Codex fix: a version
     * that did so passed every leg above, whose passes all land on a
     * deadline). */
    g_nvs_counters = SavedCounters{};
    g_first_counters = 0;
    g_first_counter_fails = 0;
    boot();
    TestMember l;
    make_member(l, mac);
    deliver(heartbeat(l), s0);
    const uint64_t l_first = l.counter;
    assert(g_first_counters == 1 && saved_counter(l) == l_first);
    deliver(heartbeat(l), s0 + kBeatGapMin);
    const uint64_t l_own = l.counter;
    g_first_counter_fails = SIZE_MAX;
    idle(s0 + 3 * R);   /* the first pass since that heartbeat */
    assert(g_first_counters == 2 && saved_counter(l) == l_first);   /* refused */
    for (uint32_t dt = 3 * R + pass_ms; dt < 4 * R; dt += pass_ms) {
      idle(s0 + dt);
      assert(g_first_counters == 2);   /* none on the passes after it */
    }
    g_first_counter_fails = 0;
    idle(s0 + 4 * R - 1);   /* to the millisecond: R after that pass, not earlier */
    assert(g_first_counters == 2);
    idle(s0 + 4 * R);
    assert(g_first_counters == 3 && saved_counter(l) == l_own);
    for (uint32_t dt = 4 * R + pass_ms; dt <= 6 * R; dt += pass_ms) idle(s0 + dt);
    assert(g_first_counters == 3 && saved_counter(l) == l_own);   /* landed: disarmed */

    /* Every call from the deadline on refused: once per R, exactly. */
    g_nvs_counters = SavedCounters{};
    g_first_counters = 0;
    g_first_counter_fails = 0;
    boot();
    TestMember p;
    make_member(p, mac);
    deliver(heartbeat(p), s0);
    const uint64_t p_first = p.counter;
    assert(g_first_counters == 1 && saved_counter(p) == p_first);
    g_first_counter_fails = SIZE_MAX;
    for (uint32_t dt = pass_ms; dt <= 10 * R; dt += pass_ms) {
      if (dt % kBeatGapMin == 0) deliver(heartbeat(p), s0 + dt);
      else                       idle(s0 + dt);
      assert(g_first_counters == 1 + dt / R);
    }
    assert(saved_counter(p) == p_first);
    g_first_counter_fails = 0;
    return g_first_counters - 1;
  };
  persistent_calls = run(1000);
  assert(run(0u - 30000u) == persistent_calls);
  assert(persistent_calls == 10);
  std::printf("PASS test_a_failed_second_save_is_tried_again_a_deadline_later"
              "  (a refused second save retried %u ms later, and %u ms after a pass that came"
              " late; every call refused: %zu retries in %u ms, one per %u ms, at t0 and across"
              " the wrap)\n",
              (unsigned)R, (unsigned)R, persistent_calls, (unsigned)(10 * R), (unsigned)R);
}

int main() {
  std::srand(0xC51F0);
  /* F161: first, before anything publishes a status view. */
  test_a_read_before_any_publish_is_the_state_before_init();
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
  test_alert_reads_stay_whole_while_the_main_loop_stores_and_clears();
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
  test_a_reflected_confirm_fails_the_pairing_through_the_session();
  test_a_reflected_confirm_fails_the_joiners_pairing_through_the_session();
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
  test_boot_restore_keeps_a_refused_entry();
  /* F70 */
  test_pair_contact_replay_records_nothing_and_gets_no_accept();
  test_copied_member_address_moves_no_link();
  test_forgetting_a_peer_drops_only_its_own_address();
  test_unbound_member_frame_is_never_taken();
  test_opera_sends_with_only_a_pairing_partner_reach_nobody();
  test_opera_sends_reach_bound_members_only();
  test_opera_sends_count_only_what_the_transport_took();
  /* F118 — a pairing whose partner this device cannot hold fails. */
  test_can_hold_partner_refusals();
  test_a_partner_the_initiator_cannot_hold_fails_the_pairing();
  test_the_initiator_asks_again_before_it_seals();
  test_a_joiner_that_cannot_hold_its_initiator_fails_the_pairing();
  /* F133 — GET /api/mesh reports the last pairing's outcome. */
  test_build_mesh_status_json_reports_the_last_pairing();
  test_status_json_fits_worst_case();
  test_get_mesh_tells_each_pairing_outcome();
  /* F134 — the initiator sends a lost COMPLETE again. */
  test_a_lost_complete_is_sent_again_through_the_session();
  test_complete_copies_stop_when_they_can_no_longer_help();
  test_complete_copies_reaching_a_paired_joiner_do_nothing();
  test_only_the_joiners_own_fresh_frame_ends_the_copies();
  test_a_leave_or_removal_in_a_copys_pass_sends_no_copy();
  /* F135 — a cancel leaves an ended pairing alone. */
  test_a_cancel_after_the_complete_still_reports_paired();
  test_a_cancel_on_a_failed_pairing_fires_nothing_again();
  /* F33 part 3 — the outbound counter survives a reboot. */
  test_outbound_counter_reserve_ahead();
  test_outbound_counter_without_reservation_restarts();
  /* F33 part 5 — the pairing routes run on the main loop. */
  test_rest_pairing_requests();
  test_rest_pair_start_creates_opera();
  /* F136 — a Canary in an opera leaves it before it joins one. */
  test_a_join_waits_for_a_leave();
  /* F162 — a member announces itself once, after it pairs and after boot. */
  test_two_paired_canaries_hear_each_other_at_once();
  test_a_reboot_announces_itself_once_to_each_member();
  /* F33 part 6 — the revocation deny-list; concurrent removals. */
  test_revocation_deny_list();
  test_concurrent_offer_propagates_and_yields();
  /* F161 — the status routes read a view the main loop publishes. */
  test_a_status_read_is_the_last_published_pass();
  test_a_read_right_after_a_post_shows_what_it_did();
  test_a_disabled_session_still_publishes_each_pass();
  test_deinit_publishes_the_wiped_session();
  test_the_view_holds_the_code_only_while_it_is_shown();
  test_the_peer_list_joins_each_member_from_the_view();
  /* F199 — the peer list is the members the device holds. */
  test_the_peer_list_is_the_members_the_device_holds();
  test_status_reads_stay_whole_while_the_main_loop_runs();
  /* F324 — a periodic heartbeat, and liveness from verified frames only. */
  test_a_lost_complete_is_healed_by_the_next_heartbeat();
  test_every_heartbeat_re_arms_the_timer();
  test_no_periodic_heartbeat_without_a_running_opera();
  test_the_heartbeat_timer_survives_a_long_disable();
  test_liveness_counts_only_verified_frames();
  test_a_long_gone_member_stays_offline_past_the_clock_wrap();
  test_the_heartbeat_spacing_holds_across_the_millis_wrap();
  test_each_heartbeat_deadline_is_drawn_afresh();
  test_a_member_bound_after_weeks_alone_is_heard_within_one_interval();
  test_a_frame_the_receiver_has_not_heard_still_counts_as_live();
  test_a_members_first_counter_is_saved_at_once();
  test_an_older_first_frame_is_saved_over_once_more();
  test_a_failed_first_counter_save_is_tried_again();
  test_a_failed_second_save_is_tried_again_a_deadline_later();
  std::printf("\nALL MESH_SESSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
