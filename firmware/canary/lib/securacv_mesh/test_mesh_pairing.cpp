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
 *   5. Symmetric mutual-DH pairing: two simulated peers run the host
 *      X25519 shim, derive the same session_key, and therefore compute
 *      the same confirmation code — the only property the user
 *      visually verifies.
 *   6. Wire-format struct sizes match the static_asserts in the header
 *      (a runtime check duplicating the compile-time assert so a CI
 *      log surface flags this loudly if the header gets edited).
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
  assert(mesh_crypto::ed25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::ed25519_generate_keypair(pub_b, priv_b));

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
  test_handshake_aborts_on_tampered_confirm_hash();
  test_timeout_after_5_minutes();
  test_cancel_wipes_state();
  test_receive_unexpected_message_is_dropped();
  std::printf("\nALL MESH_PAIRING TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
