/**
 * @file test_mesh_pairing.cpp
 * @brief Host-build conformance test for mesh_pairing primitives.
 *
 * Verifies:
 *   1. compute_confirmation_code is deterministic.
 *   2. Distinct session keys produce distinct codes.
 *   3. Code is always in [0, 999_999].
 *   4. Wire-compat regression: session_key = 32×0x00 produces code
 *      884555 (independently computed via openssl).
 *   5. Symmetric mutual-DH pairing: two simulated peers run X25519 on
 *      real X25519 keypairs, derive the same session_key, and therefore
 *      compute the same confirmation code — the only property the user
 *      visually verifies. The host X25519 is the RFC 7748 ladder, not a
 *      shim (F33 part 2): test_pairing_codes_match_through_real_x25519
 *      drives the state machine's own keygen + DH on independently
 *      generated keys, so Ed25519-generated ephemerals (the bug pairing
 *      shipped with) fail it.
 *   6. Wire-format struct sizes match the static_asserts in the header
 *      (a runtime check duplicating the compile-time assert so a CI
 *      log surface flags this loudly if the header gets edited).
 *   7. The owners confirm in either order (F97): the initiator keeps a
 *      joiner's early CONFIRM, every COMPLETE goes out with the
 *      initiator's own CONFIRM in front of it (so a joiner on the pre-F97
 *      rules completes too), the joiner takes the COMPLETE once its own
 *      owner confirmed, and a CONFIRM counts only from the partner's
 *      address and once the code is shown.
 *   8. An updated joiner re-sends its CONFIRM, bounded, once it has read
 *      the initiator's CONFIRM after its own owner confirmed (F117), so a
 *      pre-F97 initiator completes in either order and two updated devices
 *      exchange the same frames as before.
 *   9. A partner the PartnerGate refuses fails the pairing (F118): at the
 *      owner's confirm, before the initiator seals, before the joiner
 *      opens; and every NOTIFY_FAILED carries its reason.
 *  10. cancel() ends only a running pairing (F135): after the COMPLETE went
 *      out the pairing stays PAIRED and the initiator's NOTIFY_PAIRED still
 *      fires; a FAILED one keeps its reason and reports nothing again.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_pairing.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_pairing.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_pairing && /tmp/test_mesh_pairing
 */

#include "mesh_pairing.h"
#include "mesh_crypto.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_pairing_run() { return 0; }
#else

namespace {

void test_code_deterministic() {
  uint8_t session_key[mesh_pairing::SESSION_KEY_LEN];
  for (size_t i = 0; i < sizeof(session_key); ++i) session_key[i] = (uint8_t)(i * 7);
  uint32_t a = mesh_pairing::compute_confirmation_code(session_key);
  uint32_t b = mesh_pairing::compute_confirmation_code(session_key);
  assert(a == b);
  assert(a < mesh_pairing::CONFIRMATION_CODE_MODULUS);
  std::printf("PASS test_code_deterministic  (code=%06u)\n", a);
}

void test_code_distinct_inputs_distinct_outputs() {
  uint8_t k1[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint8_t k2[mesh_pairing::SESSION_KEY_LEN] = {0};
  k2[0] = 1;   /* differ in one bit */
  uint32_t c1 = mesh_pairing::compute_confirmation_code(k1);
  uint32_t c2 = mesh_pairing::compute_confirmation_code(k2);
  assert(c1 != c2);  /* 1-in-10^6 chance of accidental collision */
  std::printf("PASS test_code_distinct_inputs_distinct_outputs  (c1=%06u c2=%06u)\n", c1, c2);
}

void test_code_range() {
  /* Sweep 256 different keys and assert every output stays in the
   * declared range. Cheap regression for off-by-one modulus bugs. */
  for (int seed = 0; seed < 256; ++seed) {
    uint8_t k[mesh_pairing::SESSION_KEY_LEN];
    for (size_t i = 0; i < sizeof(k); ++i) k[i] = (uint8_t)((seed * 13 + i) & 0xFF);
    uint32_t c = mesh_pairing::compute_confirmation_code(k);
    assert(c < mesh_pairing::CONFIRMATION_CODE_MODULUS);
  }
  std::printf("PASS test_code_range  (256 seeds, all in [0, 999999])\n");
}

void test_wire_compat_code_zero_session_key() {
  /* Pinned regression: a 32-byte zero session key produces code 884555.
   * Independently computed:
   *   $ { printf 'securacv:pair:confirm:v0'; head -c 32 /dev/zero; } \
   *       | openssl dgst -sha256
   *   = 3b460b... (first 3 bytes: 0x3b, 0x46, 0x0b)
   *   top24 = 0x3b460b = 3884555
   *   3884555 %% 1000000 = 884555
   *
   * If this fails the domain string or modulus has drifted from
   * canary-wap — paired canary + canary-wap nodes would display
   * different 6-digit codes and pairing would fail user verification. */
  uint8_t zero_key[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint32_t code = mesh_pairing::compute_confirmation_code(zero_key);
  if (code != 884555u) {
    std::printf("  got code=%06u, expected 884555\n", code);
    assert(false);
  }
  std::printf("PASS test_wire_compat_code_zero_session_key  (code=%06u)\n", code);
}

void test_symmetric_mutual_dh_produces_same_code() {
  /* End-to-end check: simulate two peers, each derives the same
   * session_key via x25519, each computes the same confirmation code.
   * This is the property the pairing UI relies on for the user to
   * visually verify both screens display the same 6 digits. */
  uint8_t pub_a[mesh_crypto::PUBKEY_LEN], priv_a[mesh_crypto::PRIVKEY_LEN];
  uint8_t pub_b[mesh_crypto::PUBKEY_LEN], priv_b[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::x25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::x25519_generate_keypair(pub_b, priv_b));

  uint8_t session_a[mesh_pairing::SESSION_KEY_LEN];
  uint8_t session_b[mesh_pairing::SESSION_KEY_LEN];
  assert(mesh_crypto::x25519_derive(priv_a, pub_b, session_a));
  assert(mesh_crypto::x25519_derive(priv_b, pub_a, session_b));
  assert(std::memcmp(session_a, session_b, mesh_pairing::SESSION_KEY_LEN) == 0);

  uint32_t code_a = mesh_pairing::compute_confirmation_code(session_a);
  uint32_t code_b = mesh_pairing::compute_confirmation_code(session_b);
  assert(code_a == code_b);
  std::printf("PASS test_symmetric_mutual_dh_produces_same_code  (code=%06u)\n", code_a);
}

void test_confirmation_hash_wire_compat() {
  /* Pinned regression: session_key = 32×0x00, code = 884555 (per
   * test_wire_compat_code_zero_session_key) → confirmation_hash =
   *
   *   68b54b5271a8b39345ec536813944be01297d0619b844619a50759c624431ebf
   *
   * Computed independently:
   *   $ { printf 'securacv:pair:confirm:v0'; head -c 32 /dev/zero;
   *       printf '\x4b\x7f\x0d\x00'; } | openssl dgst -sha256
   *
   * If this fails the LE byte order, the domain string, or the input
   * concat layout has drifted from canary-wap and pairing will fail
   * the MITM-detection check cross-lane. */
  uint8_t zero_key[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint8_t got[mesh_crypto::SHA256_OUT_LEN];
  mesh_pairing::compute_confirmation_hash(zero_key, 884555u, got);

  static const uint8_t expected[mesh_crypto::SHA256_OUT_LEN] = {
    0x68, 0xb5, 0x4b, 0x52, 0x71, 0xa8, 0xb3, 0x93,
    0x45, 0xec, 0x53, 0x68, 0x13, 0x94, 0x4b, 0xe0,
    0x12, 0x97, 0xd0, 0x61, 0x9b, 0x84, 0x46, 0x19,
    0xa5, 0x07, 0x59, 0xc6, 0x24, 0x43, 0x1e, 0xbf,
  };
  if (std::memcmp(got, expected, sizeof(expected)) != 0) {
    std::printf("  got:      ");
    for (size_t i = 0; i < sizeof(got); ++i) std::printf("%02x", got[i]);
    std::printf("\n  expected: ");
    for (size_t i = 0; i < sizeof(expected); ++i) std::printf("%02x", expected[i]);
    std::printf("\n");
    assert(false);
  }
  std::printf("PASS test_confirmation_hash_wire_compat  (68b54b52...)\n");
}

void test_confirmation_hash_distinguishes_code() {
  /* Two different codes with the SAME session_key must produce
   * different hashes. Mirrors the MITM-protection property: an
   * attacker who only learns session_key (e.g. via X25519
   * eavesdrop-and-forward) still can't fake the confirm hash without
   * also knowing the code. */
  uint8_t key[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint8_t h1[mesh_crypto::SHA256_OUT_LEN], h2[mesh_crypto::SHA256_OUT_LEN];
  mesh_pairing::compute_confirmation_hash(key, 123456u, h1);
  mesh_pairing::compute_confirmation_hash(key, 123457u, h2);
  assert(std::memcmp(h1, h2, sizeof(h1)) != 0);
  std::printf("PASS test_confirmation_hash_distinguishes_code\n");
}

void test_wire_format_struct_sizes() {
  /* Mirror the static_asserts in mesh_pairing.h at runtime so a CI
   * log surface flags the drift loudly instead of just a compile fail. */
  assert(sizeof(mesh_pairing::PairDiscoverPayload) ==
         mesh_crypto::PUBKEY_LEN + (mesh_pairing::MAX_PEER_NAME_LEN + 1) + 1);
  assert(sizeof(mesh_pairing::PairOfferPayload) ==
         mesh_crypto::PUBKEY_LEN * 2 + (mesh_pairing::MAX_OPERA_NAME_LEN + 1) + 1);
  assert(sizeof(mesh_pairing::PairAcceptPayload) ==
         sizeof(mesh_pairing::PairOfferPayload));   /* aliased per canary-wap */
  assert(sizeof(mesh_pairing::PairConfirmPayload) == mesh_crypto::SHA256_OUT_LEN);
  assert(sizeof(mesh_pairing::PairCompletePayload) ==
         (mesh_crypto::OPERA_SECRET_LEN + mesh_crypto::AEAD_TAG_LEN) +
          mesh_crypto::AEAD_NONCE_LEN);
  std::printf("PASS test_wire_format_struct_sizes  (Discover=%zu Offer=%zu Confirm=%zu Complete=%zu)\n",
              sizeof(mesh_pairing::PairDiscoverPayload),
              sizeof(mesh_pairing::PairOfferPayload),
              sizeof(mesh_pairing::PairConfirmPayload),
              sizeof(mesh_pairing::PairCompletePayload));
}

}  /* namespace */

/* ── State-machine tests (PR 2e) ──────────────────────────────────────── */

namespace {

/* Two-peer simulation harness. Owns one PairingContext per side and a
 * tiny in-flight queue so process steps can be advanced deterministically. */

struct InFlight {
  uint8_t to[6];
  mesh_pairing::MsgType type;
  std::vector<uint8_t> bytes;
};

/* Map an outgoing Action to a queued InFlight + a corresponding MsgType. */
bool action_to_inflight(const mesh_pairing::Action& a, InFlight* out) {
  using mesh_pairing::ActionType;
  using mesh_pairing::MsgType;
  switch (a.type) {
    case ActionType::BROADCAST_DISCOVER:
      std::memcpy(out->to, a.peer_mac, 6); out->type = MsgType::DISCOVER; break;
    case ActionType::SEND_OFFER:
      std::memcpy(out->to, a.peer_mac, 6); out->type = MsgType::OFFER;    break;
    case ActionType::SEND_ACCEPT:
      std::memcpy(out->to, a.peer_mac, 6); out->type = MsgType::ACCEPT;   break;
    case ActionType::SEND_CONFIRM:
      std::memcpy(out->to, a.peer_mac, 6); out->type = MsgType::CONFIRM;  break;
    case ActionType::SEND_COMPLETE:
      std::memcpy(out->to, a.peer_mac, 6); out->type = MsgType::COMPLETE; break;
    default: return false;
  }
  out->bytes.assign(a.payload, a.payload + a.payload_len);
  return true;
}

/* Hard assert with a guaranteed-evaluated condition: action_to_inflight has
 * a side effect (fills the InFlight), so it must run — and the check must
 * still abort — even when compiled under NDEBUG. */
void must(bool ok) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: must() condition failed\n");
    std::abort();
  }
}

/* The initiator's CONFIRM that goes in front of a COMPLETE (F97), as the
 * frame the session sends first. False when the action carries none. */
bool leading_confirm_to_inflight(const mesh_pairing::Action& a, InFlight* out) {
  if (a.type != mesh_pairing::ActionType::SEND_COMPLETE || !a.leading_confirm_present) return false;
  std::memcpy(out->to, a.peer_mac, 6);
  out->type = mesh_pairing::MsgType::CONFIRM;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&a.leading_confirm);
  out->bytes.assign(b, b + sizeof(a.leading_confirm));
  return true;
}

void test_full_handshake_succeeds() {
  /* Two contexts, two long-term keypairs, two MACs. Drive the full
   * 5-message handshake (matching canary-wap semantics: initiator
   * handles JOINER's discover, joiner handles initiator's OFFER) and
   * assert the joiner ends up holding the initiator's opera_secret. */
  mesh_pairing::PairingContext ctx_init, ctx_join;
  mesh_pairing::context_init(ctx_init);
  mesh_pairing::context_init(ctx_join);

  uint8_t pub_i[mesh_crypto::PUBKEY_LEN], priv_i[mesh_crypto::PRIVKEY_LEN];
  uint8_t pub_j[mesh_crypto::PUBKEY_LEN], priv_j[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
  assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));

  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(opera_secret); ++i) opera_secret[i] = (uint8_t)(0x40 + i);

  const uint8_t mac_i[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  const uint8_t mac_j[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02};

  /* 1. Both sides start — each broadcasts its own DISCOVER (with its
   * role). The initiator's discover is informational; only the joiner's
   * discover triggers the OFFER on the initiator side, matching
   * canary-wap mesh_network.cpp:756. */
  mesh_pairing::Action a;
  a = mesh_pairing::start_initiator(ctx_init, pub_i, priv_i, opera_secret,
                                    "MyOpera", /*now_ms=*/100);
  assert(a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER);
  InFlight disc_i; must(action_to_inflight(a, &disc_i));

  a = mesh_pairing::start_joiner(ctx_join, pub_j, priv_j, /*now_ms=*/100);
  assert(a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER);
  InFlight disc_j; must(action_to_inflight(a, &disc_j));

  /* 2. Joiner receives initiator's DISCOVER (role=INIT). canary-wap
   * semantics: joiner ignores this since it's not a JOINER discover.
   * Our impl returns NONE. */
  a = mesh_pairing::receive(ctx_join, mac_i, disc_i.type,
                            disc_i.bytes.data(), disc_i.bytes.size(), 150);
  assert(a.type == mesh_pairing::ActionType::NONE);

  /* 3. Initiator receives joiner's DISCOVER (role=JOIN) → sends OFFER. */
  a = mesh_pairing::receive(ctx_init, mac_j, disc_j.type,
                            disc_j.bytes.data(), disc_j.bytes.size(), 200);
  assert(a.type == mesh_pairing::ActionType::SEND_OFFER);
  InFlight offer; must(action_to_inflight(a, &offer));
  assert(std::memcmp(offer.to, mac_j, 6) == 0);

  /* 4. Joiner receives OFFER → derives session key + code, emits ACCEPT. */
  a = mesh_pairing::receive(ctx_join, mac_i, offer.type,
                            offer.bytes.data(), offer.bytes.size(), 300);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  assert(a.confirmation_code != 0);
  uint32_t code_join = a.confirmation_code;
  InFlight accept; must(action_to_inflight(a, &accept));
  assert(std::memcmp(accept.to, mac_i, 6) == 0);

  /* 5. Initiator receives ACCEPT → derives session key + code → NOTIFY_CODE_READY. */
  a = mesh_pairing::receive(ctx_init, mac_j, accept.type,
                            accept.bytes.data(), accept.bytes.size(), 400);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  assert(a.confirmation_code == code_join);  /* CRITICAL: both sides agree */
  uint32_t code_init = a.confirmation_code;

  /* 6. Both users tap "confirm" → each emits SEND_CONFIRM. */
  a = mesh_pairing::confirm_code(ctx_init, 500);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  InFlight conf_i; must(action_to_inflight(a, &conf_i));

  a = mesh_pairing::confirm_code(ctx_join, 500);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  InFlight conf_j; must(action_to_inflight(a, &conf_j));

  /* 7. Initiator receives joiner's CONFIRM → sends COMPLETE. */
  a = mesh_pairing::receive(ctx_init, mac_j, conf_j.type,
                            conf_j.bytes.data(), conf_j.bytes.size(), 600);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  InFlight complete; must(action_to_inflight(a, &complete));

  /* 8. Initiator should now be PAIRED with pending NOTIFY_PAIRED — fires
   * on the next tick(). Codex P2 fix: the integration layer was
   * previously getting no explicit success signal on initiator side. */
  a = mesh_pairing::tick(ctx_init, 650);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  assert(a.confirmation_code == code_init);
  /* Subsequent ticks are idempotent: no further NOTIFY_PAIRED. */
  a = mesh_pairing::tick(ctx_init, 700);
  assert(a.type == mesh_pairing::ActionType::NONE);

  /* 9. Joiner receives initiator's CONFIRM after already having sent
   * its own → joiner has moved to AWAITING_COMPLETE so it's dropped. */
  a = mesh_pairing::receive(ctx_join, mac_i, conf_i.type,
                            conf_i.bytes.data(), conf_i.bytes.size(), 600);
  assert(a.type == mesh_pairing::ActionType::NONE);

  /* 10. Joiner receives COMPLETE → decrypts → NOTIFY_PAIRED. */
  a = mesh_pairing::receive(ctx_join, mac_i, complete.type,
                            complete.bytes.data(), complete.bytes.size(), 700);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);

  /* 11. Joiner consumes the secret. */
  uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
  assert(mesh_pairing::consume_opera_secret(ctx_join, got));
  assert(std::memcmp(got, opera_secret, mesh_crypto::OPERA_SECRET_LEN) == 0);
  assert(!mesh_pairing::consume_opera_secret(ctx_join, got));

  /* 12. session_key should have been wiped on BOTH sides after PAIRED
   * (gemini security HIGH x2). Spot-check via the public struct. */
  uint8_t zero[mesh_pairing::SESSION_KEY_LEN] = {0};
  assert(std::memcmp(ctx_init.session_key, zero, sizeof(zero)) == 0);
  assert(std::memcmp(ctx_join.session_key, zero, sizeof(zero)) == 0);

  std::printf("PASS test_full_handshake_succeeds  (code=%06u)\n", code_init);
}

/* F33 part 2 (crypto review — maintainer to confirm): two devices with
 * independently generated keys reach the SAME 6-digit code through the
 * state machine's own ephemeral keygen and x25519_derive — the real code
 * path, on the host's real X25519 (no DH mock). Also checks that each
 * offered ephemeral pub is its private scalar times the base point, i.e.
 * an X25519 key and not an Ed25519 one. Before the fix the ephemerals came
 * from ed25519_generate_keypair(): this test fails on that revision. */
void test_pairing_codes_match_through_real_x25519() {
  const uint8_t nine[32] = {9};
  for (int round = 0; round < 16; ++round) {
    mesh_pairing::PairingContext ci, cj;
    mesh_pairing::context_init(ci);
    mesh_pairing::context_init(cj);
    uint8_t pub_i[32], priv_i[32], pub_j[32], priv_j[32];
    assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
    assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));
    uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
    for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(round * 31 + i);
    const uint8_t mac_i[6] = {0x02, 0, 0, 0, 1, (uint8_t)round};
    const uint8_t mac_j[6] = {0x02, 0, 0, 0, 2, (uint8_t)round};

    mesh_pairing::Action a = mesh_pairing::start_initiator(ci, pub_i, priv_i, secret, "Home", 10);
    assert(a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER);
    a = mesh_pairing::start_joiner(cj, pub_j, priv_j, 10);
    InFlight dj; must(action_to_inflight(a, &dj));

    /* Each ephemeral is an X25519 key: pub == priv * 9. */
    uint8_t chk[32];
    assert(mesh_crypto::x25519_derive(ci.ephem_privkey, nine, chk));
    assert(std::memcmp(chk, ci.ephem_pubkey, 32) == 0);
    assert(mesh_crypto::x25519_derive(cj.ephem_privkey, nine, chk));
    assert(std::memcmp(chk, cj.ephem_pubkey, 32) == 0);
    assert(std::memcmp(ci.ephem_pubkey, cj.ephem_pubkey, 32) != 0);

    a = mesh_pairing::receive(ci, mac_j, dj.type, dj.bytes.data(), dj.bytes.size(), 20);
    InFlight of; must(action_to_inflight(a, &of));
    a = mesh_pairing::receive(cj, mac_i, of.type, of.bytes.data(), of.bytes.size(), 30);
    assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
    const uint32_t code_j = a.confirmation_code;
    InFlight ac; must(action_to_inflight(a, &ac));
    a = mesh_pairing::receive(ci, mac_j, ac.type, ac.bytes.data(), ac.bytes.size(), 40);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
    const uint32_t code_i = a.confirmation_code;

    /* The property the user checks by eye — and the session keys behind it. */
    assert(code_i == code_j);
    assert(std::memcmp(ci.session_key, cj.session_key, mesh_pairing::SESSION_KEY_LEN) == 0);
    assert(code_i == mesh_pairing::compute_confirmation_code(ci.session_key));

    /* And the handshake completes: the joiner decrypts the secret. */
    a = mesh_pairing::confirm_code(ci, 50);
    InFlight cfi; must(action_to_inflight(a, &cfi));
    a = mesh_pairing::confirm_code(cj, 50);
    InFlight cfj; must(action_to_inflight(a, &cfj));
    a = mesh_pairing::receive(ci, mac_j, cfj.type, cfj.bytes.data(), cfj.bytes.size(), 60);
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    InFlight cp; must(action_to_inflight(a, &cp));
    /* The initiator's CONFIRM verifies on the joiner (same session key). */
    a = mesh_pairing::receive(cj, mac_i, cfi.type, cfi.bytes.data(), cfi.bytes.size(), 65);
    assert(cj.state == mesh_pairing::State::AWAITING_COMPLETE);
    a = mesh_pairing::receive(cj, mac_i, cp.type, cp.bytes.data(), cp.bytes.size(), 70);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
    assert(mesh_pairing::consume_opera_secret(cj, got));
    assert(std::memcmp(got, secret, sizeof(secret)) == 0);
  }
  std::printf("PASS test_pairing_codes_match_through_real_x25519  (16 independent pairs)\n");
}

void test_handshake_aborts_on_tampered_confirm_hash() {
  /* Drive the handshake to the CONFIRM step, then corrupt the joiner's
   * confirmation_hash before the initiator receives it. Initiator must
   * transition to FAILED and NOT send COMPLETE. */
  mesh_pairing::PairingContext ctx_init, ctx_join;
  mesh_pairing::context_init(ctx_init);
  mesh_pairing::context_init(ctx_join);

  uint8_t pub_i[mesh_crypto::PUBKEY_LEN], priv_i[mesh_crypto::PRIVKEY_LEN];
  uint8_t pub_j[mesh_crypto::PUBKEY_LEN], priv_j[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
  assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN] = {0};

  const uint8_t mac_i[6] = {0xAA, 1, 0, 0, 0, 1};
  const uint8_t mac_j[6] = {0xAA, 1, 0, 0, 0, 2};

  /* Drive: init starts, join starts, init handles join's DISCOVER →
   * SEND_OFFER, join handles OFFER → SEND_ACCEPT, init handles ACCEPT
   * → NOTIFY_CODE_READY (joiner already had it). */
  mesh_pairing::Action a = mesh_pairing::start_initiator(ctx_init, pub_i, priv_i,
                                                         secret, "X", 100);
  InFlight di; action_to_inflight(a, &di); (void)di;  /* ignored */
  a = mesh_pairing::start_joiner(ctx_join, pub_j, priv_j, 100);
  InFlight dj; action_to_inflight(a, &dj);
  a = mesh_pairing::receive(ctx_init, mac_j, dj.type, dj.bytes.data(), dj.bytes.size(), 200);
  InFlight of; action_to_inflight(a, &of);
  a = mesh_pairing::receive(ctx_join, mac_i, of.type, of.bytes.data(), of.bytes.size(), 300);
  InFlight ac; action_to_inflight(a, &ac);
  a = mesh_pairing::receive(ctx_init, mac_j, ac.type, ac.bytes.data(), ac.bytes.size(), 400);
  /* Initiator now AWAITING_CONFIRM. Joiner also AWAITING_CONFIRM. */

  a = mesh_pairing::confirm_code(ctx_join, 500);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  InFlight cf; action_to_inflight(a, &cf);

  /* Tamper one bit in the confirmation_hash payload. */
  cf.bytes[3] ^= 0x01;

  /* Initiator confirms its code first to enter AWAITING_CONFIRM_PEER. */
  a = mesh_pairing::confirm_code(ctx_init, 500);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);

  /* Feed initiator the TAMPERED joiner-confirm. */
  a = mesh_pairing::receive(ctx_init, mac_j, cf.type,
                            cf.bytes.data(), cf.bytes.size(), 600);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
  /* Subsequent tick() should NOT emit a stale NOTIFY_PAIRED. */
  a = mesh_pairing::tick(ctx_init, 700);
  assert(a.type == mesh_pairing::ActionType::NONE);
  std::printf("PASS test_handshake_aborts_on_tampered_confirm_hash\n");
}

void test_timeout_after_5_minutes() {
  mesh_pairing::PairingContext ctx;
  mesh_pairing::context_init(ctx);
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  uint8_t secret[32] = {0};
  mesh_pairing::start_initiator(ctx, pub, priv, secret, "X", /*now_ms=*/1000);

  /* Just before timeout. */
  mesh_pairing::Action a = mesh_pairing::tick(ctx, 1000 + mesh_pairing::PAIRING_TIMEOUT_MS - 1);
  assert(a.type == mesh_pairing::ActionType::NONE);

  /* At + past timeout. */
  a = mesh_pairing::tick(ctx, 1000 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);

  /* Idempotent: another tick after failure is NONE. */
  a = mesh_pairing::tick(ctx, 1000 + mesh_pairing::PAIRING_TIMEOUT_MS + 1000);
  assert(a.type == mesh_pairing::ActionType::NONE);
  std::printf("PASS test_timeout_after_5_minutes\n");
}

void test_cancel_wipes_state() {
  mesh_pairing::PairingContext ctx;
  mesh_pairing::context_init(ctx);
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  uint8_t secret[32] = {1, 2, 3, 4, 5};
  mesh_pairing::start_initiator(ctx, pub, priv, secret, "X", 0);

  mesh_pairing::Action a = mesh_pairing::cancel(ctx);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
  /* ephem_privkey and opera_secret should be all-zero after cancel. */
  uint8_t zero[32] = {0};
  assert(std::memcmp(ctx.ephem_privkey, zero, 32) == 0);
  assert(std::memcmp(ctx.opera_secret, zero, 32) == 0);
  std::printf("PASS test_cancel_wipes_state\n");
}

void test_receive_unexpected_message_is_dropped() {
  mesh_pairing::PairingContext ctx;
  mesh_pairing::context_init(ctx);
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_pairing::start_joiner(ctx, pub, priv, 0);

  /* Joiner is in DISCOVERING_JOINER. Feeding it a COMPLETE should be NONE. */
  uint8_t bogus_complete[sizeof(mesh_pairing::PairCompletePayload)] = {0};
  const uint8_t mac[6] = {0xCC, 0, 0, 0, 0, 1};
  mesh_pairing::Action a = mesh_pairing::receive(ctx, mac, mesh_pairing::MsgType::COMPLETE,
                                                  bogus_complete, sizeof(bogus_complete), 100);
  assert(a.type == mesh_pairing::ActionType::NONE);
  /* And the joiner should NOT have transitioned to FAILED — stray
   * messages don't abort pairing. */
  assert(ctx.state == mesh_pairing::State::DISCOVERING_JOINER);
  std::printf("PASS test_receive_unexpected_message_is_dropped\n");
}

/* ── F97 — the owners confirm in either order ─────────────────────────────
 *
 * Until F97 either_handle_confirm acted only in AWAITING_CONFIRM_PEER (this
 * side's own owner already confirmed), dropped an earlier CONFIRM, and
 * nothing ever re-sent one; the joiner took a COMPLETE only in
 * AWAITING_COMPLETE, reached only through the initiator's CONFIRM. So with
 * frames delivered as they are sent, no order completed: the joiner's owner
 * first left both sides at the 5-minute timeout, and the initiator's owner
 * first left the initiator PAIRED with a joiner that dropped the COMPLETE.
 * The F75 fix on canary-wap carries over (spec §5.2). */

/* Two contexts driven through DISCOVER/OFFER/ACCEPT: both show the code
 * (AWAITING_CONFIRM), nothing confirmed yet. */
struct Pair {
  mesh_pairing::PairingContext ci, cj;
  uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x01};
  uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x02};
  uint8_t mac_x[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x0E};   /* a third radio */
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  uint32_t code = 0;
};

void pair_to_code(Pair& p) {
  mesh_pairing::context_init(p.ci);
  mesh_pairing::context_init(p.cj);
  uint8_t pub_i[32], priv_i[32], pub_j[32], priv_j[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
  assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));
  for (size_t i = 0; i < sizeof(p.secret); ++i) p.secret[i] = (uint8_t)(0x97 ^ (i * 5));
  mesh_pairing::Action a = mesh_pairing::start_initiator(p.ci, pub_i, priv_i, p.secret, "Home", 10);
  assert(a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER);
  a = mesh_pairing::start_joiner(p.cj, pub_j, priv_j, 10);
  InFlight dj; must(action_to_inflight(a, &dj));
  a = mesh_pairing::receive(p.ci, p.mac_j, dj.type, dj.bytes.data(), dj.bytes.size(), 20);
  InFlight of; must(action_to_inflight(a, &of));
  a = mesh_pairing::receive(p.cj, p.mac_i, of.type, of.bytes.data(), of.bytes.size(), 30);
  assert(a.type == mesh_pairing::ActionType::SEND_ACCEPT);
  InFlight ac; must(action_to_inflight(a, &ac));
  a = mesh_pairing::receive(p.ci, p.mac_j, ac.type, ac.bytes.data(), ac.bytes.size(), 40);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  p.code = a.confirmation_code;
  assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM);
  assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM);
}

mesh_pairing::Action deliver(mesh_pairing::PairingContext& to, const uint8_t from[6],
                             const InFlight& f, uint32_t now) {
  return mesh_pairing::receive(to, from, f.type, f.bytes.data(), f.bytes.size(), now);
}

/* The joiner's secret matches the initiator's, and neither side times out. */
void expect_both_paired(Pair& p, uint32_t now) {
  uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
  assert(mesh_pairing::consume_opera_secret(p.cj, got));
  assert(std::memcmp(got, p.secret, sizeof(got)) == 0);
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  assert(p.cj.state == mesh_pairing::State::PAIRED);
  mesh_pairing::Action a = mesh_pairing::tick(p.ci, now);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  assert(a.confirmation_code == p.code);
  assert(mesh_pairing::tick(p.ci, now + mesh_pairing::PAIRING_TIMEOUT_MS).type ==
         mesh_pairing::ActionType::NONE);
  assert(mesh_pairing::tick(p.cj, now + mesh_pairing::PAIRING_TIMEOUT_MS).type ==
         mesh_pairing::ActionType::NONE);
}

/* The joiner's owner confirms first. The initiator keeps the CONFIRM and,
 * at its own owner's confirm, sends the COMPLETE, with its own CONFIRM in
 * front of it. The joiner checks that CONFIRM and takes the COMPLETE; it
 * takes the COMPLETE without it too (the leading CONFIRM lost). On the
 * code before F97 the initiator dropped the joiner's CONFIRM, its
 * confirm_code sent a CONFIRM, and both sides failed at the timeout. */
void test_the_joiners_owner_may_confirm_first() {
  uint32_t code = 0;
  for (int lose_leading_confirm = 0; lose_leading_confirm < 2; ++lose_leading_confirm) {
    Pair p;
    pair_to_code(p);
    code = p.code;
    mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
    assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
    InFlight cfj; must(action_to_inflight(a, &cfj));
    assert(std::memcmp(cfj.to, p.mac_i, 6) == 0);

    /* Kept, not acted on: the initiator's owner has not confirmed. */
    a = deliver(p.ci, p.mac_j, cfj, 60);
    assert(a.type == mesh_pairing::ActionType::NONE);
    assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM);
    assert(p.ci.peer_confirmed);

    /* The owner confirms: the COMPLETE, its own CONFIRM in front. */
    a = mesh_pairing::confirm_code(p.ci, 70);
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    assert(std::memcmp(a.peer_mac, p.mac_j, 6) == 0);
    assert(p.ci.state == mesh_pairing::State::PAIRED);
    assert(!p.ci.peer_confirmed);
    InFlight lead; must(leading_confirm_to_inflight(a, &lead));
    assert(std::memcmp(lead.to, p.mac_j, 6) == 0);
    assert(lead.bytes == cfj.bytes);            /* the hash is the same both ways */
    InFlight cp; must(action_to_inflight(a, &cp));
    /* A second confirm sends nothing more. */
    assert(mesh_pairing::confirm_code(p.ci, 71).type == mesh_pairing::ActionType::NONE);

    assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
    if (!lose_leading_confirm) {
      assert(deliver(p.cj, p.mac_i, lead, 75).type == mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
    }
    a = deliver(p.cj, p.mac_i, cp, 80);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    expect_both_paired(p, 90);
  }
  std::printf("PASS test_the_joiners_owner_may_confirm_first  (code=%06u; the leading CONFIRM heard and lost)\n", code);
}

/* The initiator's owner confirms first. Its CONFIRM reaches a joiner whose
 * owner has not confirmed; the joiner checks it and needs nothing else from
 * it, and takes the COMPLETE after its own owner confirms. On the code
 * before F97 the joiner dropped that CONFIRM, then waited for it in
 * AWAITING_CONFIRM_PEER and dropped the COMPLETE: the initiator reported
 * PAIRED and the joiner failed at the timeout. */
void test_the_initiators_owner_may_confirm_first() {
  for (int lose_initiator_confirm = 0; lose_initiator_confirm < 2; ++lose_initiator_confirm) {
    Pair p;
    pair_to_code(p);
    mesh_pairing::Action a = mesh_pairing::confirm_code(p.ci, 50);
    assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
    InFlight cfi; must(action_to_inflight(a, &cfi));
    assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
    if (!lose_initiator_confirm) {
      a = deliver(p.cj, p.mac_i, cfi, 60);
      assert(a.type == mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM);   /* still the owner's call */
    }

    a = mesh_pairing::confirm_code(p.cj, 70);
    assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
    InFlight cfj; must(action_to_inflight(a, &cfj));
    a = deliver(p.ci, p.mac_j, cfj, 80);
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    InFlight lead; must(leading_confirm_to_inflight(a, &lead));
    assert(lead.bytes == cfi.bytes);            /* the CONFIRM again, in front */
    InFlight cp; must(action_to_inflight(a, &cp));

    assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
    if (lose_initiator_confirm) {
      /* This time the joiner reads the CONFIRM in front of the COMPLETE. */
      assert(deliver(p.cj, p.mac_i, lead, 85).type == mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
    }
    a = deliver(p.cj, p.mac_i, cp, 90);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    expect_both_paired(p, 100);
  }
  std::printf("PASS test_the_initiators_owner_may_confirm_first  (the initiator's CONFIRM heard and lost)\n");
}

/* Unchanged: with the initiator's owner confirmed and no CONFIRM from the
 * joiner, the initiator sends nothing more and fails at the timeout. */
void test_the_initiator_still_waits_for_the_joiners_confirm() {
  Pair p;
  pair_to_code(p);
  assert(mesh_pairing::confirm_code(p.ci, 50).type == mesh_pairing::ActionType::SEND_CONFIRM);
  assert(mesh_pairing::confirm_code(p.ci, 51).type == mesh_pairing::ActionType::NONE);
  assert(mesh_pairing::tick(p.ci, 60).type == mesh_pairing::ActionType::NONE);
  assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
  assert(mesh_pairing::tick(p.ci, 10 + mesh_pairing::PAIRING_TIMEOUT_MS).type ==
         mesh_pairing::ActionType::NOTIFY_FAILED);
  std::printf("PASS test_the_initiator_still_waits_for_the_joiners_confirm\n");
}

/* A CONFIRM counts only from the partner's address. Here a third radio
 * sends the joiner's correct hash (ESP-NOW frames are not encrypted, so a
 * radio that overheard it has it) before the initiator's owner confirms: it
 * is not kept, the owner's confirm sends a CONFIRM, not a COMPLETE, and the
 * pairing still completes with the real joiner. A wrong hash from that
 * radio does not end the pairing either, before or after the owner's
 * confirm. Both fail with the address check removed. */
void test_a_confirm_from_another_address_does_not_count() {
  Pair p;
  pair_to_code(p);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
  InFlight cfj; must(action_to_inflight(a, &cfj));

  a = deliver(p.ci, p.mac_x, cfj, 60);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(!p.ci.peer_confirmed);
  InFlight bad = cfj;
  bad.bytes[0] ^= 0x01;
  a = deliver(p.ci, p.mac_x, bad, 61);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM);

  a = mesh_pairing::confirm_code(p.ci, 70);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  InFlight cfi; must(action_to_inflight(a, &cfi));
  a = deliver(p.ci, p.mac_x, bad, 71);
  assert(a.type == mesh_pairing::ActionType::NONE);
  a = deliver(p.ci, p.mac_x, cfj, 72);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(p.ci.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);

  /* The joiner, too, ignores a CONFIRM from the third radio. */
  a = deliver(p.cj, p.mac_x, bad, 73);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);

  /* The real joiner's CONFIRM completes it. */
  a = deliver(p.ci, p.mac_j, cfj, 80);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  InFlight cp; must(action_to_inflight(a, &cp));
  assert(deliver(p.cj, p.mac_i, cfi, 85).type == mesh_pairing::ActionType::NONE);
  assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
  assert(deliver(p.cj, p.mac_i, cp, 90).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  expect_both_paired(p, 100);
  std::printf("PASS test_a_confirm_from_another_address_does_not_count\n");
}

/* A CONFIRM counts only once the code is shown. Before the ACCEPT the
 * initiator's session key and code are all zero, so anyone can compute
 * that CONFIRM's hash; one sent from the partner's own address then must
 * not count later. Fails with the state check removed (the initiator then
 * seals the opera_secret under the all-zero key at once). */
void test_a_confirm_before_the_code_is_shown_does_not_count() {
  mesh_pairing::PairingContext ci, cj;
  mesh_pairing::context_init(ci);
  mesh_pairing::context_init(cj);
  uint8_t pub_i[32], priv_i[32], pub_j[32], priv_j[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
  assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  std::memset(secret, 0x5C, sizeof(secret));
  const uint8_t mac_i[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x21};
  const uint8_t mac_j[6] = {0x24, 0x0A, 0xC4, 0x00, 0x97, 0x22};
  mesh_pairing::start_initiator(ci, pub_i, priv_i, secret, "Home", 10);
  mesh_pairing::Action a = mesh_pairing::start_joiner(cj, pub_j, priv_j, 10);
  InFlight dj; must(action_to_inflight(a, &dj));
  a = deliver(ci, mac_j, dj, 20);
  InFlight of; must(action_to_inflight(a, &of));
  assert(ci.state == mesh_pairing::State::AWAITING_ACCEPT);

  /* The all-zero CONFIRM, from the partner's address. */
  const uint8_t zero_key[mesh_pairing::SESSION_KEY_LEN] = {0};
  InFlight early;
  std::memcpy(early.to, mac_i, 6);
  early.type = mesh_pairing::MsgType::CONFIRM;
  early.bytes.resize(sizeof(mesh_pairing::PairConfirmPayload));
  mesh_pairing::compute_confirmation_hash(zero_key, ci.confirmation_code, early.bytes.data());
  a = deliver(ci, mac_j, early, 25);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(ci.state == mesh_pairing::State::AWAITING_ACCEPT);
  assert(!ci.peer_confirmed);

  a = deliver(cj, mac_i, of, 30);
  InFlight ac; must(action_to_inflight(a, &ac));
  a = deliver(ci, mac_j, ac, 40);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  /* The owner's confirm waits for the joiner's real CONFIRM. */
  a = mesh_pairing::confirm_code(ci, 50);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  assert(ci.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
  std::printf("PASS test_a_confirm_before_the_code_is_shown_does_not_count\n");
}

/* A wrong hash from the partner's address ends the pairing in either order,
 * on either side: until F97 one that arrived before the owner's confirm was
 * dropped unread. */
void test_a_bad_confirm_from_the_partner_ends_the_pairing_in_either_order() {
  for (int side = 0; side < 2; ++side) {
    Pair p;
    pair_to_code(p);
    mesh_pairing::PairingContext& to = side == 0 ? p.ci : p.cj;
    const uint8_t* from = side == 0 ? p.mac_j : p.mac_i;
    mesh_pairing::Action a = mesh_pairing::confirm_code(side == 0 ? p.cj : p.ci, 50);
    InFlight cf; must(action_to_inflight(a, &cf));
    cf.bytes[7] ^= 0x80;
    assert(to.state == mesh_pairing::State::AWAITING_CONFIRM);   /* before its owner's */
    a = deliver(to, from, cf, 60);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
    assert(to.state == mesh_pairing::State::FAILED);
    assert(!to.peer_confirmed);
    /* Wiped, and the owner's confirm now does nothing. */
    uint8_t zero[mesh_pairing::SESSION_KEY_LEN] = {0};
    assert(std::memcmp(to.session_key, zero, sizeof(zero)) == 0);
    assert(mesh_pairing::confirm_code(to, 70).type == mesh_pairing::ActionType::NONE);
  }
  std::printf("PASS test_a_bad_confirm_from_the_partner_ends_the_pairing_in_either_order\n");
}

/* The joiner takes a COMPLETE only after its own owner confirmed, and only
 * a joiner takes one. The COMPLETE here comes from an initiator that took
 * the joiner's hash from somewhere other than the joiner (the reflection
 * F94 names): a joiner whose owner has not confirmed drops it, and takes
 * the same COMPLETE once its owner has. An initiator drops a COMPLETE at
 * every step. */
void test_the_joiner_takes_a_complete_only_after_its_owner_confirms() {
  Pair p;
  pair_to_code(p);
  InFlight forged;
  std::memcpy(forged.to, p.mac_i, 6);
  forged.type = mesh_pairing::MsgType::CONFIRM;
  forged.bytes.resize(sizeof(mesh_pairing::PairConfirmPayload));
  mesh_pairing::compute_confirmation_hash(p.cj.session_key, p.cj.confirmation_code,
                                          forged.bytes.data());
  assert(deliver(p.ci, p.mac_j, forged, 50).type == mesh_pairing::ActionType::NONE);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.ci, 60);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  InFlight lead; must(leading_confirm_to_inflight(a, &lead));
  InFlight cp; must(action_to_inflight(a, &cp));

  /* The CONFIRM in front of it is checked and changes nothing here. */
  a = deliver(p.cj, p.mac_i, lead, 65);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM);
  a = deliver(p.cj, p.mac_i, cp, 70);
  assert(a.type == mesh_pairing::ActionType::NONE);
  assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM);
  assert(!p.cj.opera_secret_present);

  a = mesh_pairing::confirm_code(p.cj, 80);
  assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
  a = deliver(p.cj, p.mac_i, cp, 90);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);

  /* An initiator never takes a COMPLETE: not before its owner's confirm,
   * not after it. (Without the role check a garbled one from the partner's
   * address would end its pairing.) */
  Pair q;
  pair_to_code(q);
  InFlight junk;
  std::memcpy(junk.to, q.mac_i, 6);
  junk.type = mesh_pairing::MsgType::COMPLETE;
  junk.bytes.assign(sizeof(mesh_pairing::PairCompletePayload), 0xA5);
  assert(deliver(q.ci, q.mac_j, junk, 50).type == mesh_pairing::ActionType::NONE);
  assert(mesh_pairing::confirm_code(q.ci, 60).type == mesh_pairing::ActionType::SEND_CONFIRM);
  assert(deliver(q.ci, q.mac_j, junk, 70).type == mesh_pairing::ActionType::NONE);
  assert(q.ci.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
  std::printf("PASS test_the_joiner_takes_a_complete_only_after_its_owner_confirms\n");
}

/* A joiner on firmware before F97 (c6a305b's rules), modeled on the
 * current joiner: it reads a CONFIRM only once its own owner confirmed
 * (AWAITING_CONFIRM_PEER), which moves it to AWAITING_COMPLETE, and takes a
 * COMPLETE only there. Anything else it drops unread. On the honest path
 * that is exactly the old handlers' behavior. */
mesh_pairing::Action pre_f97_joiner_receive(mesh_pairing::PairingContext& cj,
                                            const uint8_t from[6], const InFlight& f,
                                            uint32_t now) {
  if (f.type == mesh_pairing::MsgType::CONFIRM &&
      cj.state != mesh_pairing::State::AWAITING_CONFIRM_PEER) {
    return mesh_pairing::Action{};
  }
  if (f.type == mesh_pairing::MsgType::COMPLETE &&
      cj.state != mesh_pairing::State::AWAITING_COMPLETE) {
    return mesh_pairing::Action{};
  }
  return deliver(cj, from, f, now);
}

/* An updated initiator pairs a joiner on firmware before F97, in either
 * order, because every COMPLETE goes out with the initiator's CONFIRM in
 * front of it. Frames are delivered as they are sent: the leading CONFIRM,
 * then the COMPLETE. Fails with the COMPLETE sent alone: such a joiner
 * waits in AWAITING_CONFIRM_PEER for a CONFIRM read after its owner's
 * confirm and drops the COMPLETE, while the initiator reports PAIRED (and
 * main.cpp would register, bind and store a member that never joined).
 * Host-checked against the real c6a305b handlers too (scratch probe). */
void test_a_pre_f97_joiner_completes_in_either_order() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    Pair p;
    pair_to_code(p);
    mesh_pairing::Action a;
    InFlight lead, cp;
    if (joiner_first) {
      a = mesh_pairing::confirm_code(p.cj, 50);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
      a = mesh_pairing::confirm_code(p.ci, 70);
    } else {
      a = mesh_pairing::confirm_code(p.ci, 50);
      InFlight cfi; must(action_to_inflight(a, &cfi));
      /* Dropped unread: this joiner's owner has not confirmed. */
      assert(pre_f97_joiner_receive(p.cj, p.mac_i, cfi, 55).type ==
             mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM);
      a = mesh_pairing::confirm_code(p.cj, 60);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      a = deliver(p.ci, p.mac_j, cfj, 70);
    }
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    const bool has_lead = leading_confirm_to_inflight(a, &lead);
    must(action_to_inflight(a, &cp));
    assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
    /* As sent: the leading CONFIRM (when there is one), then the COMPLETE. */
    if (has_lead) {
      assert(pre_f97_joiner_receive(p.cj, p.mac_i, lead, 80).type ==
             mesh_pairing::ActionType::NONE);
    }
    a = pre_f97_joiner_receive(p.cj, p.mac_i, cp, 90);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);   /* not dropped */
    expect_both_paired(p, 100);
  }
  std::printf("PASS test_a_pre_f97_joiner_completes_in_either_order\n");
}

/* ── F117 — an updated joiner pairs a pre-F97 initiator in either order ──
 *
 * An initiator on firmware before F97 (c6a305b's rules) reads a CONFIRM only
 * once its own owner confirmed (AWAITING_CONFIRM_PEER) and answers it with a
 * COMPLETE alone; one that arrives earlier it drops unread. The updated
 * joiner sends its CONFIRM at its owner's confirm, so when the joiner's
 * owner confirmed first that CONFIRM was dropped, and both sides timed out.
 * Now the joiner re-sends it once it has read the initiator's CONFIRM after
 * its own owner confirmed: CONFIRM_RESEND_FIRST_MS later, then every
 * CONFIRM_RESEND_INTERVAL_MS, at most CONFIRM_RESEND_MAX, only to the
 * partner, only while it waits for the COMPLETE. */

/* The pre-F97 initiator, modeled on the current one: a CONFIRM before its
 * owner's confirm is dropped unread, and its COMPLETE goes alone (the old
 * Action had no leading CONFIRM). On the honest path that is exactly the
 * old handlers' behavior; the scratch probe for F117 also ran the real
 * c6a305b library against this one. */
mesh_pairing::Action pre_f97_initiator_receive(mesh_pairing::PairingContext& ci,
                                               const uint8_t from[6], const InFlight& f,
                                               uint32_t now) {
  if (f.type == mesh_pairing::MsgType::CONFIRM &&
      ci.state != mesh_pairing::State::AWAITING_CONFIRM_PEER) {
    return mesh_pairing::Action{};
  }
  return deliver(ci, from, f, now);
}

/* Both orders with a pre-F97 initiator. Joiner's owner first: its CONFIRM is
 * dropped; the initiator's owner confirms and its CONFIRM moves the joiner
 * to AWAITING_COMPLETE; the joiner's tick re-sends its CONFIRM after
 * CONFIRM_RESEND_FIRST_MS (not before); the initiator answers with the
 * COMPLETE alone, and both are PAIRED with the secret. Initiator's owner
 * first: completes on the first CONFIRM, nothing re-sent. Fails with the
 * re-send not armed (both sides then time out, as before F117). */
void test_a_pre_f97_initiator_completes_in_either_order() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    Pair p;
    pair_to_code(p);
    mesh_pairing::Action a;
    InFlight cfj;
    if (joiner_first) {
      a = mesh_pairing::confirm_code(p.cj, 50);
      must(action_to_inflight(a, &cfj));
      assert(pre_f97_initiator_receive(p.ci, p.mac_j, cfj, 55).type ==
             mesh_pairing::ActionType::NONE);              /* dropped unread */
      assert(!p.ci.peer_confirmed);
      a = mesh_pairing::confirm_code(p.ci, 1000);
      assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
      InFlight cfi; must(action_to_inflight(a, &cfi));
      assert(deliver(p.cj, p.mac_i, cfi, 1010).type == mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
      /* Nothing before the delay; then the same CONFIRM, to the partner. */
      assert(mesh_pairing::tick(p.cj, 1010 + mesh_pairing::CONFIRM_RESEND_FIRST_MS - 1).type ==
             mesh_pairing::ActionType::NONE);
      a = mesh_pairing::tick(p.cj, 1010 + mesh_pairing::CONFIRM_RESEND_FIRST_MS);
      assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
      InFlight again; must(action_to_inflight(a, &again));
      assert(std::memcmp(again.to, p.mac_i, 6) == 0);
      assert(again.bytes == cfj.bytes);
      a = pre_f97_initiator_receive(p.ci, p.mac_j, again, 2020);
    } else {
      a = mesh_pairing::confirm_code(p.ci, 50);
      InFlight cfi; must(action_to_inflight(a, &cfi));
      assert(deliver(p.cj, p.mac_i, cfi, 55).type == mesh_pairing::ActionType::NONE);
      a = mesh_pairing::confirm_code(p.cj, 60);
      must(action_to_inflight(a, &cfj));
      a = pre_f97_initiator_receive(p.ci, p.mac_j, cfj, 70);
    }
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    InFlight cp; must(action_to_inflight(a, &cp));    /* alone: no leading CONFIRM */
    a = deliver(p.cj, p.mac_i, cp, 2030);
    assert(a.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    expect_both_paired(p, 2040);
    /* PAIRED: the re-send is over. */
    assert(mesh_pairing::tick(p.cj, 2040 + 10 * mesh_pairing::CONFIRM_RESEND_INTERVAL_MS).type ==
           mesh_pairing::ActionType::NONE);
  }
  std::printf("PASS test_a_pre_f97_initiator_completes_in_either_order\n");
}

/* The re-send is bounded and goes only where it should: with nobody
 * answering, a joiner in AWAITING_COMPLETE sends exactly
 * CONFIRM_RESEND_MAX copies, at FIRST then INTERVAL spacing, all to the
 * partner, then nothing until the timeout. A joiner whose owner confirmed
 * but that has not read the initiator's CONFIRM (AWAITING_CONFIRM_PEER)
 * re-sends nothing, and neither does an initiator. Fails with the bound
 * removed. */
void test_the_joiners_confirm_resend_is_bounded() {
  Pair p;
  pair_to_code(p);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
  InFlight cfj; must(action_to_inflight(a, &cfj));
  assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM_PEER);
  a = mesh_pairing::confirm_code(p.ci, 60);   /* the joiner's CONFIRM was never delivered */
  InFlight cfi; must(action_to_inflight(a, &cfi));
  for (uint32_t t = 60; t < 60 + 60000; t += 100) {
    assert(mesh_pairing::tick(p.cj, t).type == mesh_pairing::ActionType::NONE);
    assert(mesh_pairing::tick(p.ci, t).type == mesh_pairing::ActionType::NONE);
  }
  const uint32_t t0 = 70000;
  assert(deliver(p.cj, p.mac_i, cfi, t0).type == mesh_pairing::ActionType::NONE);
  std::vector<uint32_t> sent_at;
  for (uint32_t t = t0; t < 10 + mesh_pairing::PAIRING_TIMEOUT_MS; t += 50) {
    a = mesh_pairing::tick(p.cj, t);
    if (a.type == mesh_pairing::ActionType::SEND_CONFIRM) {
      InFlight f; must(action_to_inflight(a, &f));
      assert(std::memcmp(f.to, p.mac_i, 6) == 0 && f.bytes == cfj.bytes);
      sent_at.push_back(t - t0);
    } else {
      assert(a.type == mesh_pairing::ActionType::NONE);
    }
    assert(mesh_pairing::tick(p.ci, t).type == mesh_pairing::ActionType::NONE);
  }
  assert(sent_at.size() == mesh_pairing::CONFIRM_RESEND_MAX);
  for (size_t i = 0; i < sent_at.size(); ++i) {
    assert(sent_at[i] == mesh_pairing::CONFIRM_RESEND_FIRST_MS +
                         (uint32_t)i * mesh_pairing::CONFIRM_RESEND_INTERVAL_MS);
  }
  a = mesh_pairing::tick(p.cj, 10 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
  std::printf("PASS test_the_joiners_confirm_resend_is_bounded  (%zu copies)\n", sent_at.size());
}

/* Two updated devices are unchanged by the joiner's re-send: in both
 * orders, every frame delivered as sent and both sides ticked every 50 ms,
 * the joiner sends one CONFIRM, as before F117: the COMPLETE lands before
 * a re-send is due. A copy that did go out would reach an initiator
 * already PAIRED, which drops it (checked at the end). The initiator sends
 * its COMPLETE (with its CONFIRM in front) once before the joiner is
 * PAIRED; what it sends after are F134's copies, every
 * COMPLETE_RESEND_INTERVAL_MS, each byte for byte the first, which the
 * PAIRED joiner drops (nothing here plays the session that hears the
 * joiner and ends them). */
void test_two_updated_devices_resend_nothing() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    Pair p;
    pair_to_code(p);
    struct Q { bool to_j; InFlight f; };
    std::vector<Q> air;
    int confirms_from_j = 0, completes = 0, completes_before_paired = 0;
    std::vector<uint8_t> first_complete;
    auto put = [&](bool from_i, const mesh_pairing::Action& a) {
      InFlight lead, f;
      if (leading_confirm_to_inflight(a, &lead)) air.push_back(Q{from_i, lead});
      if (action_to_inflight(a, &f)) {
        if (!from_i && f.type == mesh_pairing::MsgType::CONFIRM) ++confirms_from_j;
        if (f.type == mesh_pairing::MsgType::COMPLETE) {
          ++completes;
          if (p.cj.state != mesh_pairing::State::PAIRED) ++completes_before_paired;
          if (first_complete.empty()) first_complete = f.bytes;
          assert(f.bytes == first_complete);           /* a copy, not a new seal */
          assert(std::memcmp(f.to, p.mac_j, 6) == 0);
        }
        air.push_back(Q{from_i, f});
      }
    };
    auto drain = [&](uint32_t t) {
      for (size_t k = 0; k < air.size(); ++k) {
        const Q q = air[k];
        put(!q.to_j, q.to_j ? deliver(p.cj, p.mac_i, q.f, t) : deliver(p.ci, p.mac_j, q.f, t));
      }
      air.clear();
    };
    const uint32_t first = 100, second = 3100;
    for (uint32_t t = 100; t < 20000; t += 50) {
      if (t == first)  put(!joiner_first, mesh_pairing::confirm_code(joiner_first ? p.cj : p.ci, t));
      if (t == second) put(joiner_first, mesh_pairing::confirm_code(joiner_first ? p.ci : p.cj, t));
      drain(t);
      mesh_pairing::Action ti = mesh_pairing::tick(p.ci, t);
      if (ti.type == mesh_pairing::ActionType::NOTIFY_PAIRED) continue;
      put(true, ti);
      put(false, mesh_pairing::tick(p.cj, t));
      drain(t);
    }
    assert(p.ci.state == mesh_pairing::State::PAIRED && p.cj.state == mesh_pairing::State::PAIRED);
    assert(confirms_from_j == 1 && completes_before_paired == 1);
    assert(completes == 1 + (int)p.ci.complete_copies && p.ci.complete_copies > 0);
    uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
    assert(mesh_pairing::consume_opera_secret(p.cj, got));
    assert(std::memcmp(got, p.secret, sizeof(got)) == 0);
  }
  /* A late copy reaches a PAIRED initiator: dropped. */
  Pair p;
  pair_to_code(p);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
  InFlight cfj; must(action_to_inflight(a, &cfj));
  assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
  assert(mesh_pairing::confirm_code(p.ci, 70).type == mesh_pairing::ActionType::SEND_COMPLETE);
  assert(deliver(p.ci, p.mac_j, cfj, 80).type == mesh_pairing::ActionType::NONE);
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  std::printf("PASS test_two_updated_devices_resend_nothing  (both orders)\n");
}

/* ── F118 — a device that cannot hold its partner fails the pairing ──────
 *
 * The integration layer's PartnerGate (mesh_session::can_hold_partner on a
 * device) is asked at this side's owner's confirm, before any CONFIRM goes
 * out; on the initiator again before the opera_secret is sealed; on the
 * joiner again before a COMPLETE is opened. Until F118 nothing was asked:
 * the initiator sealed the secret and both sides reported PAIRED, and only
 * then did the session find it could not bind the partner. */

/* A gate the test flips, recording what it was asked. */
bool                 g_gate_admits = true;
int                  g_gate_calls  = 0;
std::vector<uint8_t> g_gate_pub, g_gate_mac;
bool test_gate(const uint8_t pub[mesh_crypto::PUBKEY_LEN], const uint8_t mac[6]) {
  ++g_gate_calls;
  g_gate_pub.assign(pub, pub + mesh_crypto::PUBKEY_LEN);
  g_gate_mac.assign(mac, mac + 6);
  return g_gate_admits;
}

/* pair_to_code() with the test gate on both sides; the long-term keys are
 * returned so the gate's arguments can be checked. */
void pair_to_code_gated(Pair& p, uint8_t pub_i[32], uint8_t pub_j[32]) {
  mesh_pairing::context_init(p.ci);
  mesh_pairing::context_init(p.cj);
  uint8_t priv_i[32], priv_j[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_i, priv_i));
  assert(mesh_crypto::ed25519_generate_keypair(pub_j, priv_j));
  for (size_t i = 0; i < sizeof(p.secret); ++i) p.secret[i] = (uint8_t)(0x18 ^ (i * 3));
  mesh_pairing::Action a =
      mesh_pairing::start_initiator(p.ci, pub_i, priv_i, p.secret, "Home", 10, test_gate);
  a = mesh_pairing::start_joiner(p.cj, pub_j, priv_j, 10, test_gate);
  InFlight dj; must(action_to_inflight(a, &dj));
  a = deliver(p.ci, p.mac_j, dj, 20);
  InFlight of; must(action_to_inflight(a, &of));
  a = deliver(p.cj, p.mac_i, of, 30);
  InFlight ac; must(action_to_inflight(a, &ac));
  a = deliver(p.ci, p.mac_j, ac, 40);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_CODE_READY);
  p.code = a.confirmation_code;
  g_gate_calls = 0;   /* nothing asks it before a confirm */
}

void expect_refused(const mesh_pairing::Action& a, const mesh_pairing::PairingContext& c) {
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
  assert(a.fail_reason == mesh_pairing::FailReason::PARTNER_REFUSED);
  assert(c.state == mesh_pairing::State::FAILED);
  assert(c.fail_reason == mesh_pairing::FailReason::PARTNER_REFUSED);
  const uint8_t zero[mesh_pairing::SESSION_KEY_LEN] = {0};
  assert(std::memcmp(c.session_key, zero, sizeof(zero)) == 0);
  assert(!c.opera_secret_present);
}

/* Refused at this side's owner's confirm, on either role: NOTIFY_FAILED
 * (PARTNER_REFUSED) in place of the CONFIRM, nothing sent, the keys wiped,
 * and the gate asked with the partner's long-term key and address. A
 * refusing joiner sends no CONFIRM, so its initiator never seals the
 * secret to it: it waits and fails at the timeout. Fails with the gate
 * removed from confirm_code (the CONFIRM goes out). */
void test_a_refused_partner_fails_at_the_owners_confirm() {
  for (int joiner_side = 0; joiner_side < 2; ++joiner_side) {
    Pair p;
    uint8_t pub_i[32], pub_j[32];
    pair_to_code_gated(p, pub_i, pub_j);
    mesh_pairing::PairingContext& me = joiner_side ? p.cj : p.ci;
    g_gate_admits = false;
    mesh_pairing::Action a = mesh_pairing::confirm_code(me, 50);
    expect_refused(a, me);
    assert(g_gate_calls == 1);
    assert(std::memcmp(g_gate_pub.data(), joiner_side ? pub_i : pub_j, 32) == 0);
    assert(std::memcmp(g_gate_mac.data(), joiner_side ? p.mac_i : p.mac_j, 6) == 0);
    assert(std::strcmp(mesh_pairing::fail_reason_name(a.fail_reason), "partner_refused") == 0);
    /* Over: a second confirm does nothing; the tick reports nothing more. */
    assert(mesh_pairing::confirm_code(me, 51).type == mesh_pairing::ActionType::NONE);
    assert(mesh_pairing::tick(me, 10 + mesh_pairing::PAIRING_TIMEOUT_MS).type ==
           mesh_pairing::ActionType::NONE);
    if (joiner_side) {
      /* The initiator's owner confirms: its CONFIRM goes, no COMPLETE ever
       * does, and it fails at the timeout. */
      g_gate_admits = true;
      assert(mesh_pairing::confirm_code(p.ci, 60).type == mesh_pairing::ActionType::SEND_CONFIRM);
      for (uint32_t t = 70; t < 10 + mesh_pairing::PAIRING_TIMEOUT_MS; t += 10000) {
        assert(mesh_pairing::tick(p.ci, t).type == mesh_pairing::ActionType::NONE);
      }
      a = mesh_pairing::tick(p.ci, 10 + mesh_pairing::PAIRING_TIMEOUT_MS);
      assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED);
      assert(a.fail_reason == mesh_pairing::FailReason::TIMEOUT);
    }
  }
  g_gate_admits = true;
  std::printf("PASS test_a_refused_partner_fails_at_the_owners_confirm  (both roles)\n");
}

/* The initiator asks again before it seals the opera_secret. Initiator's
 * owner first: its confirm is admitted and its CONFIRM goes; the gate then
 * refuses (the tables changed), and the joiner's CONFIRM gets NOTIFY_FAILED
 * instead of the COMPLETE. Joiner's owner first: its CONFIRM is kept, and
 * the initiator's owner's confirm, refused, seals nothing either. Fails
 * with the gate removed from initiator_complete (the first order seals and
 * returns SEND_COMPLETE). */
void test_the_initiator_seals_nothing_to_a_refused_partner() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    Pair p;
    uint8_t pub_i[32], pub_j[32];
    pair_to_code_gated(p, pub_i, pub_j);
    mesh_pairing::Action a;
    if (joiner_first) {
      a = mesh_pairing::confirm_code(p.cj, 50);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
      assert(p.ci.peer_confirmed);
      g_gate_admits = false;
      a = mesh_pairing::confirm_code(p.ci, 70);
    } else {
      a = mesh_pairing::confirm_code(p.ci, 50);
      assert(a.type == mesh_pairing::ActionType::SEND_CONFIRM);
      a = mesh_pairing::confirm_code(p.cj, 60);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      g_gate_admits = false;
      a = deliver(p.ci, p.mac_j, cfj, 70);
    }
    expect_refused(a, p.ci);
    assert(!a.leading_confirm_present);
    assert(!p.ci.pending_notify_paired);
    assert(mesh_pairing::tick(p.ci, 80).type == mesh_pairing::ActionType::NONE);
    g_gate_admits = true;
  }
  std::printf("PASS test_the_initiator_seals_nothing_to_a_refused_partner  (both orders)\n");
}

/* The joiner asks again before it opens a COMPLETE: admitted at its
 * owner's confirm, refused when the COMPLETE arrives. NOTIFY_FAILED, the
 * secret neither opened nor handed over. Fails with the gate removed from
 * joiner_handle_complete (NOTIFY_PAIRED with the secret). */
void test_the_joiner_opens_nothing_from_a_refused_partner() {
  Pair p;
  uint8_t pub_i[32], pub_j[32];
  pair_to_code_gated(p, pub_i, pub_j);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
  InFlight cfj; must(action_to_inflight(a, &cfj));
  a = mesh_pairing::confirm_code(p.ci, 60);
  InFlight cfi; must(action_to_inflight(a, &cfi));
  a = deliver(p.ci, p.mac_j, cfj, 70);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  InFlight cp; must(action_to_inflight(a, &cp));
  assert(deliver(p.cj, p.mac_i, cfi, 75).type == mesh_pairing::ActionType::NONE);
  g_gate_admits = false;
  g_gate_calls = 0;
  a = deliver(p.cj, p.mac_i, cp, 80);
  expect_refused(a, p.cj);
  assert(g_gate_calls == 1);
  uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
  assert(!mesh_pairing::consume_opera_secret(p.cj, got));
  g_gate_admits = true;
  std::printf("PASS test_the_joiner_opens_nothing_from_a_refused_partner\n");
}

/* Every NOTIFY_FAILED says why, and the context keeps it. */
void test_every_failure_says_why() {
  using mesh_pairing::FailReason;
  Pair p;
  pair_to_code(p);
  mesh_pairing::Action a = mesh_pairing::cancel(p.ci);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED && a.fail_reason == FailReason::CANCELED);
  assert(p.ci.fail_reason == FailReason::CANCELED);
  a = mesh_pairing::tick(p.cj, 10 + mesh_pairing::PAIRING_TIMEOUT_MS);
  assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED && a.fail_reason == FailReason::TIMEOUT);

  Pair q;
  pair_to_code(q);
  a = mesh_pairing::confirm_code(q.cj, 50);
  InFlight bad; must(action_to_inflight(a, &bad));
  bad.bytes[3] ^= 0x10;
  a = deliver(q.ci, q.mac_j, bad, 60);
  assert(a.fail_reason == FailReason::BAD_CONFIRM && q.ci.fail_reason == FailReason::BAD_CONFIRM);

  Pair r;
  pair_to_code(r);
  assert(mesh_pairing::confirm_code(r.cj, 50).type == mesh_pairing::ActionType::SEND_CONFIRM);
  InFlight junk;
  std::memcpy(junk.to, r.mac_j, 6);
  junk.type = mesh_pairing::MsgType::COMPLETE;
  junk.bytes.assign(sizeof(mesh_pairing::PairCompletePayload), 0x3C);
  a = deliver(r.cj, r.mac_i, junk, 60);
  assert(a.fail_reason == FailReason::BAD_COMPLETE);

  /* Success and every other action carry NONE; a fresh context has none. */
  Pair s;
  pair_to_code(s);
  assert(s.ci.fail_reason == FailReason::NONE);
  a = mesh_pairing::confirm_code(s.ci, 50);
  assert(a.fail_reason == FailReason::NONE);
  assert(std::strcmp(mesh_pairing::fail_reason_name(FailReason::TIMEOUT), "timeout") == 0);
  assert(std::strcmp(mesh_pairing::fail_reason_name(FailReason::NONE), "none") == 0);
  std::printf("PASS test_every_failure_says_why\n");
}

/* ── F134 — a lost COMPLETE is sent again ─────────────────────────────────
 *
 * Nothing on the wire acknowledges a COMPLETE, and the initiator reports
 * PAIRED once it sent one. Until F134 it went once: lost on the air, it left
 * the initiator holding a member that never joined, while the joiner
 * re-sent its CONFIRM (F117) to an initiator that dropped it, and timed
 * out. Now the initiator keeps the two frames it sent and tick() sends them
 * again every COMPLETE_RESEND_INTERVAL_MS, to the partner, for at most
 * COMPLETE_RESEND_WINDOW_MS after the first send, unless the integration
 * layer ends them (stop_complete_resend). */

/* Drive a pair to the initiator's SEND_COMPLETE, either owner first, the
 * joiner's owner confirming at `t` and the other 10 ms later or earlier;
 * returns that action. */
mesh_pairing::Action pair_to_complete(Pair& p, bool joiner_first, uint32_t t) {
  pair_to_code(p);
  mesh_pairing::Action a;
  if (joiner_first) {
    a = mesh_pairing::confirm_code(p.cj, t);
    InFlight cfj; must(action_to_inflight(a, &cfj));
    assert(deliver(p.ci, p.mac_j, cfj, t).type == mesh_pairing::ActionType::NONE);
    a = mesh_pairing::confirm_code(p.ci, t + 10);
  } else {
    a = mesh_pairing::confirm_code(p.ci, t - 10);
    InFlight cfi; must(action_to_inflight(a, &cfi));
    assert(deliver(p.cj, p.mac_i, cfi, t - 10).type == mesh_pairing::ActionType::NONE);
    a = mesh_pairing::confirm_code(p.cj, t);
    InFlight cfj; must(action_to_inflight(a, &cfj));
    a = deliver(p.ci, p.mac_j, cfj, t + 10);
  }
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE && a.leading_confirm_present);
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  return a;
}

/* The COMPLETE is lost (and, in the second variant, the CONFIRM in front
 * of it too, and in the third the first copy as well), in both orders,
 * both sides ticked every 50 ms and every other frame delivered as sent.
 * The joiner's F117 re-sends reach the PAIRED initiator and change
 * nothing; the copies come at exactly the interval after the first send,
 * the same bytes, to the joiner, and the next one that arrives completes
 * it with the secret. Fails on the code before F134: no copy, and the
 * joiner times out. */
void test_a_lost_complete_is_sent_again_until_the_joiner_takes_it() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    for (int lose = 0; lose < 3; ++lose) {
      Pair p;
      const uint32_t t_c = 1000 + 10;
      mesh_pairing::Action a = pair_to_complete(p, joiner_first, 1000);
      InFlight lead0, cp0;
      must(leading_confirm_to_inflight(a, &lead0));
      must(action_to_inflight(a, &cp0));
      if (lose == 0) {   /* only the COMPLETE lost */
        mesh_pairing::Action r = deliver(p.cj, p.mac_i, lead0, t_c);
        assert(r.type == mesh_pairing::ActionType::NONE);
        assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
      }
      std::vector<uint32_t> copies_at;
      int joiner_confirms = 0;
      uint32_t paired_at = 0;
      for (uint32_t t = t_c + 50; t < t_c + 20000 && paired_at == 0; t += 50) {
        mesh_pairing::Action ti = mesh_pairing::tick(p.ci, t);
        if (ti.type == mesh_pairing::ActionType::SEND_COMPLETE) {
          InFlight lead, cp;
          must(leading_confirm_to_inflight(ti, &lead));
          must(action_to_inflight(ti, &cp));
          assert(std::memcmp(cp.to, p.mac_j, 6) == 0 && std::memcmp(lead.to, p.mac_j, 6) == 0);
          assert(cp.bytes == cp0.bytes && lead.bytes == lead0.bytes);
          copies_at.push_back(t - t_c);
          if (lose == 2 && copies_at.size() == 1) continue;   /* the first copy lost too */
          assert(deliver(p.cj, p.mac_i, lead, t).type == mesh_pairing::ActionType::NONE);
          mesh_pairing::Action r = deliver(p.cj, p.mac_i, cp, t);
          if (r.type == mesh_pairing::ActionType::NOTIFY_PAIRED) paired_at = t;
        } else {
          assert(ti.type == mesh_pairing::ActionType::NONE ||
                 ti.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
        }
        mesh_pairing::Action tj = mesh_pairing::tick(p.cj, t);
        if (tj.type == mesh_pairing::ActionType::SEND_CONFIRM) {
          ++joiner_confirms;
          InFlight f; must(action_to_inflight(tj, &f));
          assert(deliver(p.ci, p.mac_j, f, t).type == mesh_pairing::ActionType::NONE);
        } else {
          assert(tj.type == mesh_pairing::ActionType::NONE);
        }
      }
      assert(paired_at != 0);
      for (size_t i = 0; i < copies_at.size(); ++i) {
        assert(copies_at[i] == (uint32_t)(i + 1) * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
      }
      assert(copies_at.size() == (lose == 2 ? 2u : 1u));
      /* The joiner that read the leading CONFIRM re-sent its own (F117)
       * before the copy came; the initiator answered none of them. */
      assert(joiner_confirms == (lose == 0 ? 1 : 0));
      uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
      assert(mesh_pairing::consume_opera_secret(p.cj, got));
      assert(std::memcmp(got, p.secret, sizeof(got)) == 0);
      assert(mesh_pairing::complete_resend_running(p.ci));   /* nothing heard it here */
    }
  }
  std::printf("PASS test_a_lost_complete_is_sent_again_until_the_joiner_takes_it"
              "  (both orders; COMPLETE, both frames, and a copy lost)\n");
}

/* With nothing answering, the copies are bounded: one every interval after
 * the first send, the last before the window ends, so
 * (COMPLETE_RESEND_WINDOW_MS - 1) / COMPLETE_RESEND_INTERVAL_MS of them,
 * then none; the kept frames are wiped and the pairing stays PAIRED. The
 * joiner, which started before the COMPLETE, gave up before the window
 * ended. Fails with the window removed (a copy past it). */
void test_the_complete_copies_are_bounded() {
  Pair p;
  const uint32_t t_c = 5000 + 10;
  mesh_pairing::Action a = pair_to_complete(p, true, 5000);
  InFlight cp0; must(action_to_inflight(a, &cp0));
  assert(mesh_pairing::tick(p.ci, t_c).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  uint32_t copies = 0, last = 0;
  for (uint32_t t = t_c; t < t_c + 2 * mesh_pairing::COMPLETE_RESEND_WINDOW_MS; t += 100) {
    mesh_pairing::Action ti = mesh_pairing::tick(p.ci, t);
    if (ti.type == mesh_pairing::ActionType::SEND_COMPLETE) {
      InFlight cp; must(action_to_inflight(ti, &cp));
      assert(cp.bytes == cp0.bytes && std::memcmp(cp.to, p.mac_j, 6) == 0);
      ++copies;
      last = t - t_c;
    } else {
      assert(ti.type == mesh_pairing::ActionType::NONE);
    }
  }
  assert(copies == (mesh_pairing::COMPLETE_RESEND_WINDOW_MS - 1) /
                   mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
  assert(last < mesh_pairing::COMPLETE_RESEND_WINDOW_MS);
  assert(p.ci.complete_copies == copies);
  assert(!mesh_pairing::complete_resend_running(p.ci));
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  const uint8_t zero[sizeof(mesh_pairing::PairCompletePayload)] = {0};
  assert(std::memcmp(&p.ci.kept_complete, zero, sizeof(p.ci.kept_complete)) == 0);
  assert(std::memcmp(&p.ci.kept_confirm, zero, sizeof(p.ci.kept_confirm)) == 0);
  /* The joiner's own wait, from its start (10), ended inside the window. */
  assert(10 + mesh_pairing::PAIRING_TIMEOUT_MS < t_c + mesh_pairing::COMPLETE_RESEND_WINDOW_MS);
  std::printf("PASS test_the_complete_copies_are_bounded  (%u copies)\n", (unsigned)copies);
}

/* stop_complete_resend() ends the copies at once and wipes the kept
 * frames; the pairing stays PAIRED and its NOTIFY_PAIRED, still pending,
 * fires. A second call reports there was nothing to stop. A joiner never
 * has copies. */
void test_stop_complete_resend_ends_the_copies() {
  Pair p;
  const uint32_t t_c = 1000 + 10;
  mesh_pairing::Action a = pair_to_complete(p, false, 1000);
  InFlight lead, cp;
  must(leading_confirm_to_inflight(a, &lead));
  must(action_to_inflight(a, &cp));
  assert(mesh_pairing::complete_resend_running(p.ci));
  assert(mesh_pairing::stop_complete_resend(p.ci));
  assert(!mesh_pairing::complete_resend_running(p.ci));
  assert(!mesh_pairing::stop_complete_resend(p.ci));
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  assert(mesh_pairing::tick(p.ci, t_c).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  for (uint32_t t = t_c; t < t_c + 30000; t += 100) {
    assert(mesh_pairing::tick(p.ci, t).type == mesh_pairing::ActionType::NONE);
  }
  const uint8_t zero[sizeof(mesh_pairing::PairCompletePayload)] = {0};
  assert(std::memcmp(&p.ci.kept_complete, zero, sizeof(p.ci.kept_complete)) == 0);
  assert(deliver(p.cj, p.mac_i, lead, t_c).type == mesh_pairing::ActionType::NONE);
  assert(deliver(p.cj, p.mac_i, cp, t_c).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  assert(!mesh_pairing::complete_resend_running(p.cj));
  assert(!mesh_pairing::stop_complete_resend(p.cj));
  std::printf("PASS test_stop_complete_resend_ends_the_copies\n");
}

/* A pre-F97 joiner (the model above: a CONFIRM read only in
 * AWAITING_CONFIRM_PEER, a COMPLETE taken only in AWAITING_COMPLETE) whose
 * COMPLETE was lost, or both frames were: each copy carries the
 * initiator's CONFIRM in front, so it completes. A copy of the COMPLETE
 * alone would leave the both-lost case stuck in AWAITING_CONFIRM_PEER. */
void test_a_lost_complete_reaches_a_pre_f97_joiner() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    for (int lose_both = 0; lose_both < 2; ++lose_both) {
      Pair p;
      pair_to_code(p);
      mesh_pairing::Action a;
      if (joiner_first) {
        a = mesh_pairing::confirm_code(p.cj, 50);
        InFlight cfj; must(action_to_inflight(a, &cfj));
        assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
        a = mesh_pairing::confirm_code(p.ci, 70);
      } else {
        a = mesh_pairing::confirm_code(p.ci, 50);
        InFlight cfi; must(action_to_inflight(a, &cfi));
        assert(pre_f97_joiner_receive(p.cj, p.mac_i, cfi, 55).type ==
               mesh_pairing::ActionType::NONE);              /* dropped unread */
        a = mesh_pairing::confirm_code(p.cj, 60);
        InFlight cfj; must(action_to_inflight(a, &cfj));
        a = deliver(p.ci, p.mac_j, cfj, 70);
      }
      assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
      InFlight lead; must(leading_confirm_to_inflight(a, &lead));
      if (!lose_both) {
        assert(pre_f97_joiner_receive(p.cj, p.mac_i, lead, 75).type ==
               mesh_pairing::ActionType::NONE);
        assert(p.cj.state == mesh_pairing::State::AWAITING_COMPLETE);
      }
      assert(mesh_pairing::tick(p.ci, 80).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
      mesh_pairing::Action ti = mesh_pairing::tick(p.ci, 70 + mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
      assert(ti.type == mesh_pairing::ActionType::SEND_COMPLETE);
      InFlight lead2, cp2;
      must(leading_confirm_to_inflight(ti, &lead2));
      must(action_to_inflight(ti, &cp2));
      assert(pre_f97_joiner_receive(p.cj, p.mac_i, lead2, 2100).type ==
             mesh_pairing::ActionType::NONE);
      assert(pre_f97_joiner_receive(p.cj, p.mac_i, cp2, 2100).type ==
             mesh_pairing::ActionType::NOTIFY_PAIRED);
      uint8_t got[mesh_crypto::OPERA_SECRET_LEN];
      assert(mesh_pairing::consume_opera_secret(p.cj, got));
      assert(std::memcmp(got, p.secret, sizeof(got)) == 0);
    }
  }
  std::printf("PASS test_a_lost_complete_reaches_a_pre_f97_joiner  (both orders; one or both frames lost)\n");
}

/* Nothing that reaches a PAIRED initiator moves its copies anywhere but the
 * partner, or makes it send anything else: the joiner's CONFIRM replayed
 * from the joiner's address, the initiator's own CONFIRM reflected from it
 * (F94), both of them from a third radio, a third radio's DISCOVER (role
 * joiner, its own key), and an OFFER, ACCEPT and COMPLETE from either
 * address are all dropped (NONE); the copies keep their 2 s cadence from
 * the first send (no CONFIRM brings one early), keep their bytes and go to
 * the partner's address only, and the partner's key and address in the
 * context do not move. */
void test_frames_reaching_a_paired_initiator_send_the_secret_nowhere_else() {
  Pair p;
  pair_to_code(p);
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.cj, 50);
  InFlight cfj; must(action_to_inflight(a, &cfj));
  assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
  a = mesh_pairing::confirm_code(p.ci, 100);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
  InFlight lead0, cp0;
  must(leading_confirm_to_inflight(a, &lead0));
  must(action_to_inflight(a, &cp0));
  uint8_t peer_pub[32], peer_mac[6];
  std::memcpy(peer_pub, p.ci.peer_pubkey, 32);
  std::memcpy(peer_mac, p.ci.peer_mac, 6);

  /* The stray frames, built once. */
  uint8_t x_pub[32], x_priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(x_pub, x_priv));
  mesh_pairing::PairDiscoverPayload disc{};
  std::memcpy(disc.pubkey, x_pub, 32);
  disc.role = mesh_pairing::ROLE_JOINER;
  mesh_pairing::PairOfferPayload offer{};
  std::memcpy(offer.device_pubkey, x_pub, 32);
  std::memcpy(offer.ephemeral_pubkey, x_pub, 32);
  std::vector<std::pair<mesh_pairing::MsgType, std::vector<uint8_t>>> strays = {
      {mesh_pairing::MsgType::CONFIRM, cfj.bytes},     /* the joiner's, replayed */
      {mesh_pairing::MsgType::CONFIRM, lead0.bytes},   /* its own, reflected (F94) */
      {mesh_pairing::MsgType::DISCOVER,
       std::vector<uint8_t>((const uint8_t*)&disc, (const uint8_t*)&disc + sizeof(disc))},
      {mesh_pairing::MsgType::OFFER,
       std::vector<uint8_t>((const uint8_t*)&offer, (const uint8_t*)&offer + sizeof(offer))},
      {mesh_pairing::MsgType::ACCEPT,
       std::vector<uint8_t>((const uint8_t*)&offer, (const uint8_t*)&offer + sizeof(offer))},
      {mesh_pairing::MsgType::COMPLETE, cp0.bytes},
  };
  const uint8_t* froms[2] = {p.mac_j, p.mac_x};

  std::vector<uint32_t> copies_at;
  for (uint32_t t = 100; t < 100 + 3 * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS + 50; t += 50) {
    for (const uint8_t* from : froms) {
      for (const auto& s : strays) {
        mesh_pairing::Action r =
            mesh_pairing::receive(p.ci, from, s.first, s.second.data(), s.second.size(), t);
        assert(r.type == mesh_pairing::ActionType::NONE);
      }
    }
    mesh_pairing::Action ti = mesh_pairing::tick(p.ci, t);
    if (ti.type == mesh_pairing::ActionType::SEND_COMPLETE) {
      InFlight lead, cp;
      must(leading_confirm_to_inflight(ti, &lead));
      must(action_to_inflight(ti, &cp));
      assert(std::memcmp(cp.to, p.mac_j, 6) == 0 && std::memcmp(lead.to, p.mac_j, 6) == 0);
      assert(cp.bytes == cp0.bytes && lead.bytes == lead0.bytes);
      copies_at.push_back(t - 100);
    } else {
      assert(ti.type == mesh_pairing::ActionType::NONE ||
             ti.type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    }
  }
  assert(copies_at.size() == 3);
  for (size_t i = 0; i < copies_at.size(); ++i) {
    assert(copies_at[i] == (uint32_t)(i + 1) * mesh_pairing::COMPLETE_RESEND_INTERVAL_MS);
  }
  assert(std::memcmp(p.ci.peer_pubkey, peer_pub, 32) == 0);
  assert(std::memcmp(p.ci.peer_mac, peer_mac, 6) == 0);
  assert(p.ci.state == mesh_pairing::State::PAIRED);
  std::printf("PASS test_frames_reaching_a_paired_initiator_send_the_secret_nowhere_else\n");
}

/* The copies do not lower the joiner's bar: a joiner whose owner has not
 * confirmed (the initiator completed on its own CONFIRM reflected to it,
 * F94) drops every copy, and takes one only after its owner confirms,
 * inside the window. (So the reflection no longer always leaves the
 * initiator with a member that never joined: it still does when the
 * joiner's owner never confirms.) */
void test_a_copy_is_taken_only_after_the_joiners_owner_confirms() {
  Pair p;
  pair_to_code(p);
  /* The initiator's owner confirms; its CONFIRM is reflected back to it
   * from the joiner's address. */
  mesh_pairing::Action a = mesh_pairing::confirm_code(p.ci, 50);
  InFlight cfi; must(action_to_inflight(a, &cfi));
  a = deliver(p.ci, p.mac_j, cfi, 60);
  assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);   /* F94, still open */
  assert(mesh_pairing::tick(p.ci, 60).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
  int copies = 0;
  uint32_t paired_at = 0;
  const uint32_t owner_at = 30100;   /* on the 100 ms tick grid */
  for (uint32_t t = 100; t < 60 + 60000 && paired_at == 0; t += 100) {
    if (t == owner_at) {
      assert(mesh_pairing::confirm_code(p.cj, t).type == mesh_pairing::ActionType::SEND_CONFIRM);
    }
    mesh_pairing::Action ti = mesh_pairing::tick(p.ci, t);
    if (ti.type != mesh_pairing::ActionType::SEND_COMPLETE) continue;
    ++copies;
    InFlight lead, cp;
    must(leading_confirm_to_inflight(ti, &lead));
    must(action_to_inflight(ti, &cp));
    deliver(p.cj, p.mac_i, lead, t);
    mesh_pairing::Action r = deliver(p.cj, p.mac_i, cp, t);
    if (t < owner_at) {
      assert(r.type == mesh_pairing::ActionType::NONE);
      assert(p.cj.state == mesh_pairing::State::AWAITING_CONFIRM && !p.cj.opera_secret_present);
    } else if (r.type == mesh_pairing::ActionType::NOTIFY_PAIRED) {
      paired_at = t;
    }
  }
  assert(copies > 10 && paired_at >= owner_at && paired_at < owner_at + 2100);
  std::printf("PASS test_a_copy_is_taken_only_after_the_joiners_owner_confirms\n");
}

/* ── F135 — cancel() leaves an ended pairing alone ────────────────────────
 *
 * Until F135 cancel() failed every state but IDLE. A cancel that landed
 * after the initiator's COMPLETE went out turned its PAIRED context FAILED
 * (NOTIFY_FAILED, canceled; the deferred NOTIFY_PAIRED never came), though
 * the joiner had the secret; on FAILED it reported the failure again. */

/* A cancel after the initiator's SEND_COMPLETE, in both orders: NONE, the
 * context still PAIRED, and the next tick still NOTIFY_PAIRED. The joiner,
 * PAIRED with the secret, is left alone too: NONE, and the secret is still
 * there to consume. Fails on the code before F135 (NOTIFY_FAILED, canceled,
 * then NONE from the tick). */
void test_a_cancel_after_the_complete_leaves_the_pairing_paired() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    Pair p;
    pair_to_code(p);
    mesh_pairing::Action a;
    InFlight cfi;
    if (joiner_first) {
      a = mesh_pairing::confirm_code(p.cj, 50);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      assert(deliver(p.ci, p.mac_j, cfj, 60).type == mesh_pairing::ActionType::NONE);
      a = mesh_pairing::confirm_code(p.ci, 70);
    } else {
      a = mesh_pairing::confirm_code(p.ci, 50);
      must(action_to_inflight(a, &cfi));
      assert(deliver(p.cj, p.mac_i, cfi, 55).type == mesh_pairing::ActionType::NONE);
      a = mesh_pairing::confirm_code(p.cj, 60);
      InFlight cfj; must(action_to_inflight(a, &cfj));
      a = deliver(p.ci, p.mac_j, cfj, 70);
    }
    assert(a.type == mesh_pairing::ActionType::SEND_COMPLETE);
    InFlight lead, cp;
    must(leading_confirm_to_inflight(a, &lead));
    must(action_to_inflight(a, &cp));

    /* The owner's cancel lands now, before the tick that reports PAIRED. */
    mesh_pairing::Action c = mesh_pairing::cancel(p.ci);
    assert(c.type == mesh_pairing::ActionType::NONE);
    assert(c.fail_reason == mesh_pairing::FailReason::NONE);
    assert(p.ci.state == mesh_pairing::State::PAIRED);
    assert(p.ci.fail_reason == mesh_pairing::FailReason::NONE);
    assert(p.ci.pending_notify_paired);

    /* The joiner takes the COMPLETE; a cancel there is a no-op as well. */
    assert(deliver(p.cj, p.mac_i, lead, 80).type == mesh_pairing::ActionType::NONE);
    assert(deliver(p.cj, p.mac_i, cp, 80).type == mesh_pairing::ActionType::NOTIFY_PAIRED);
    assert(mesh_pairing::cancel(p.cj).type == mesh_pairing::ActionType::NONE);
    assert(p.cj.state == mesh_pairing::State::PAIRED && p.cj.opera_secret_present);
    expect_both_paired(p, 90);   /* the initiator's NOTIFY_PAIRED still fires */
    /* After it fired, too. */
    assert(mesh_pairing::cancel(p.ci).type == mesh_pairing::ActionType::NONE);
    assert(p.ci.state == mesh_pairing::State::PAIRED);
  }
  std::printf("PASS test_a_cancel_after_the_complete_leaves_the_pairing_paired  (both orders)\n");
}

/* A cancel on a FAILED pairing reports nothing again and keeps the reason
 * it failed for: a timeout, a bad CONFIRM, a refusal and a cancel each
 * stay what they were. Fails on the code before F135 (a second
 * NOTIFY_FAILED, and the reason overwritten with CANCELED). */
void test_a_cancel_on_a_failed_pairing_reports_nothing_again() {
  using mesh_pairing::FailReason;
  for (int how = 0; how < 4; ++how) {
    Pair p;
    uint8_t pub_i[32], pub_j[32];
    pair_to_code_gated(p, pub_i, pub_j);
    mesh_pairing::Action a;
    FailReason want;
    if (how == 0) {
      a = mesh_pairing::tick(p.ci, 10 + mesh_pairing::PAIRING_TIMEOUT_MS);
      want = FailReason::TIMEOUT;
    } else if (how == 1) {
      a = mesh_pairing::confirm_code(p.cj, 50);
      InFlight bad; must(action_to_inflight(a, &bad));
      bad.bytes[0] ^= 0x01;
      a = deliver(p.ci, p.mac_j, bad, 60);
      want = FailReason::BAD_CONFIRM;
    } else if (how == 2) {
      g_gate_admits = false;
      a = mesh_pairing::confirm_code(p.ci, 50);
      g_gate_admits = true;
      want = FailReason::PARTNER_REFUSED;
    } else {
      a = mesh_pairing::cancel(p.ci);
      want = FailReason::CANCELED;
    }
    assert(a.type == mesh_pairing::ActionType::NOTIFY_FAILED && a.fail_reason == want);
    assert(p.ci.state == mesh_pairing::State::FAILED);
    for (int again = 0; again < 2; ++again) {
      mesh_pairing::Action c = mesh_pairing::cancel(p.ci);
      assert(c.type == mesh_pairing::ActionType::NONE);
      assert(c.fail_reason == FailReason::NONE);
      assert(p.ci.state == mesh_pairing::State::FAILED);
      assert(p.ci.fail_reason == want);
    }
    assert(mesh_pairing::tick(p.ci, 20 + mesh_pairing::PAIRING_TIMEOUT_MS).type ==
           mesh_pairing::ActionType::NONE);
  }
  std::printf("PASS test_a_cancel_on_a_failed_pairing_reports_nothing_again  "
              "(timeout, bad_confirm, partner_refused, canceled)\n");
}

/* A cancel with nothing running: a fresh context and one that was reset
 * stay IDLE, and nothing is reported. */
void test_a_cancel_with_nothing_running_does_nothing() {
  mesh_pairing::PairingContext ctx;
  mesh_pairing::context_init(ctx);
  for (int i = 0; i < 2; ++i) {
    mesh_pairing::Action c = mesh_pairing::cancel(ctx);
    assert(c.type == mesh_pairing::ActionType::NONE);
    assert(ctx.state == mesh_pairing::State::IDLE);
    assert(ctx.fail_reason == mesh_pairing::FailReason::NONE);
  }
  std::printf("PASS test_a_cancel_with_nothing_running_does_nothing\n");
}

}  /* namespace */

int main() {
  std::srand(0xC5101);
  test_code_deterministic();
  test_code_distinct_inputs_distinct_outputs();
  test_code_range();
  test_wire_compat_code_zero_session_key();
  test_symmetric_mutual_dh_produces_same_code();
  test_confirmation_hash_wire_compat();
  test_confirmation_hash_distinguishes_code();
  test_wire_format_struct_sizes();
  test_full_handshake_succeeds();
  test_pairing_codes_match_through_real_x25519();
  test_handshake_aborts_on_tampered_confirm_hash();
  test_timeout_after_5_minutes();
  test_cancel_wipes_state();
  test_receive_unexpected_message_is_dropped();
  test_the_joiners_owner_may_confirm_first();
  test_the_initiators_owner_may_confirm_first();
  test_the_initiator_still_waits_for_the_joiners_confirm();
  test_a_confirm_from_another_address_does_not_count();
  test_a_confirm_before_the_code_is_shown_does_not_count();
  test_a_bad_confirm_from_the_partner_ends_the_pairing_in_either_order();
  test_the_joiner_takes_a_complete_only_after_its_owner_confirms();
  test_a_pre_f97_joiner_completes_in_either_order();
  test_a_pre_f97_initiator_completes_in_either_order();
  test_the_joiners_confirm_resend_is_bounded();
  test_two_updated_devices_resend_nothing();
  test_a_refused_partner_fails_at_the_owners_confirm();
  test_the_initiator_seals_nothing_to_a_refused_partner();
  test_the_joiner_opens_nothing_from_a_refused_partner();
  test_every_failure_says_why();
  test_a_lost_complete_is_sent_again_until_the_joiner_takes_it();
  test_the_complete_copies_are_bounded();
  test_stop_complete_resend_ends_the_copies();
  test_a_lost_complete_reaches_a_pre_f97_joiner();
  test_frames_reaching_a_paired_initiator_send_the_secret_nowhere_else();
  test_a_copy_is_taken_only_after_the_joiners_owner_confirms();
  test_a_cancel_after_the_complete_leaves_the_pairing_paired();
  test_a_cancel_on_a_failed_pairing_reports_nothing_again();
  test_a_cancel_with_nothing_running_does_nothing();
  std::printf("\nALL MESH_PAIRING TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
