// Host test for canary-wap's Opera pairing key agreement (mesh_pair_crypto.h),
// F33 part 2 — CRYPTO: crypto review, maintainer to confirm.
//
// The bug: the pairing handlers made their ephemeral keys with Ed25519 and ran
// X25519 over them, so the two sides derived different session keys and the
// 6-digit codes could never match. The fix is a clamped Curve25519 keypair.
//
// What this proves, through the REAL code path — mesh_pair_crypto.h compiled
// as the sketch compiles it, against real implementations of the primitives it
// calls (OpenSSL's X25519, HKDF-SHA256 and SHA-256 behind the rweather /
// mbedTLS names; stubs/pair_crypto), with no mock of the key exchange:
//   1. the stand-in X25519 reproduces RFC 7748 §6.1 (Alice/Bob), so it is the
//      function the device computes for the clamped keys generate_keypair makes;
//   2. generate_keypair clamps per RFC 7748 §5 and returns pub = priv * 9;
//   3. two independently generated keypairs derive the SAME session key both
//      ways and so the SAME 6-digit code — 32 independent pairs;
//   4. the old keys (an Ed25519 seed and its Ed25519 public key) do NOT agree —
//      the bug, reproduced, so this suite would have caught it;
//   5. the code is wire-identical to the PlatformIO tree's (a zero session key
//      gives 884555, test_mesh_pairing's pin) and a low-order peer key is
//      refused;
//   6. mesh_network.cpp's pairing handlers call this header and nothing else
//      makes pairing keys: no Ed25519 keygen left in handle_pair_discover /
//      handle_pair_offer, the code comes from confirmation_code(), and a
//      refused derivation cancels the pairing. (Source pins — the .cpp cannot
//      compile on the host; its path is passed in, so the pins fail closed.)
//   7. the CONFIRM hash names its sender's role (F94): the same four pinned
//      vectors as the PlatformIO tree's test_mesh_pairing, neither role's
//      hash is the role-free one firmware before F94 sent, and
//      mesh_network.cpp sends its own role's hash and checks its partner's,
//      with no other CONFIRM hash left in it. test_mesh_liveness_wap runs the
//      handlers: a reflected CONFIRM, and a role-free one, end the pairing.
//   8. a hash that fails is refused (sweep F364): confirmation_code() and
//      confirm_hash() answer false, with the code 0 and the hash zeroed, when
//      sha256_domain() does (the stand-in below fails the domain a case
//      names), and each of mesh_network.cpp's four callers ends the pairing
//      with an ALERT on false: handle_pair_offer before the ACCEPT goes out,
//      handle_pair_accept before the code is shown, handle_pair_confirm
//      before any compare, confirm_pairing before a CONFIRM is sent (source
//      pins). test_mesh_liveness_wap runs two devices through it: whose
//      hashes both failed, they show no code, send no CONFIRM and complete
//      no pairing.

#include "mesh_pair_crypto.h"
#include "beacon_source_scan.h"

#include <openssl/evp.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif

// The sketch's mesh_crypto::sha256_domain (mesh_crypto.cpp, mbedTLS) for the
// host: SHA-256(domain || data), here on OpenSSL, with its contract's
// refusal (sweep F364): a hash under g_fail_domain answers false with `out`
// zeroed, as the sketch's does after a failed mbedTLS step
// (test_wap_sha_fail_modules runs the sketch's own over a failing mbedTLS).
namespace pair_fail {
const char* g_fail_domain = nullptr;  // fail every hash under this domain
int g_failed = 0;                     // hashes failed since the last arm
void arm(const char* domain) {
  g_fail_domain = domain;
  g_failed = 0;
}
}  // namespace pair_fail

namespace mesh_crypto {
bool sha256_domain(const char* domain, const uint8_t* data, size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]) {
  if (pair_fail::g_fail_domain != nullptr && std::strcmp(domain, pair_fail::g_fail_domain) == 0) {
    ++pair_fail::g_failed;
    std::memset(out, 0, SHA256_OUT_LEN);
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  unsigned int len = 0;
  const bool ok = ctx != nullptr &&
                  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, domain, strlen(domain)) == 1 &&
                  (data_len == 0 || EVP_DigestUpdate(ctx, data, data_len) == 1) &&
                  EVP_DigestFinal_ex(ctx, out, &len) == 1 && len == SHA256_OUT_LEN;
  EVP_MD_CTX_free(ctx);
  if (!ok) std::abort();
  return true;
}
}  // namespace mesh_crypto

namespace {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

void from_hex(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned v = 0;
    if (std::sscanf(hex + 2 * i, "%2x", &v) != 1) std::abort();
    out[i] = (uint8_t)v;
  }
}

// esp_fill_random for the host: the kernel's RNG.
void host_fill(void* buf, size_t len) {
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (f == nullptr) std::abort();
  const size_t got = std::fread(buf, 1, len, f);
  std::fclose(f);
  if (got != len) std::abort();
}

void test_rfc7748_alice_bob_through_the_stand_in() {
  uint8_t a[32], ap[32], b[32], bp[32], k[32], out[32];
  from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
  from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", ap, 32);
  from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
  from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bp, 32);
  from_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", k, 32);
  mesh_pair_crypto::clamp_scalar(a);
  mesh_pair_crypto::clamp_scalar(b);
  CHECK(Curve25519::eval(out, a, nullptr) && std::memcmp(out, ap, 32) == 0);
  CHECK(Curve25519::eval(out, b, nullptr) && std::memcmp(out, bp, 32) == 0);
  CHECK(Curve25519::eval(out, a, bp) && std::memcmp(out, k, 32) == 0);
  CHECK(Curve25519::eval(out, b, ap) && std::memcmp(out, k, 32) == 0);
  std::printf("PASS rfc7748_alice_bob_through_the_stand_in\n");
}

void test_generate_keypair_is_clamped_x25519() {
  for (int i = 0; i < 8; ++i) {
    uint8_t pub[32], priv[32], again[32];
    CHECK(mesh_pair_crypto::generate_keypair(pub, priv, host_fill));
    CHECK((priv[0] & 0x07) == 0);
    CHECK((priv[31] & 0x80) == 0);
    CHECK((priv[31] & 0x40) != 0);
    CHECK(Curve25519::eval(again, priv, nullptr));
    CHECK(std::memcmp(again, pub, 32) == 0);
  }
  uint8_t pub[32], priv[32];
  CHECK(!mesh_pair_crypto::generate_keypair(nullptr, priv, host_fill));
  CHECK(!mesh_pair_crypto::generate_keypair(pub, nullptr, host_fill));
  CHECK(!mesh_pair_crypto::generate_keypair(pub, priv, nullptr));
  std::printf("PASS generate_keypair_is_clamped_x25519\n");
}

void test_two_sides_agree_on_session_key_and_code() {
  for (int round = 0; round < 32; ++round) {
    // Initiator and joiner, each with its own fresh ephemeral keypair — the
    // calls handle_pair_discover and handle_pair_offer make.
    uint8_t pub_i[32], priv_i[32], pub_j[32], priv_j[32];
    CHECK(mesh_pair_crypto::generate_keypair(pub_i, priv_i, host_fill));
    CHECK(mesh_pair_crypto::generate_keypair(pub_j, priv_j, host_fill));
    CHECK(std::memcmp(pub_i, pub_j, 32) != 0);

    // Joiner (handle_pair_offer) and initiator (handle_pair_accept).
    uint8_t key_j[32], key_i[32];
    CHECK(mesh_pair_crypto::derive_session_key(priv_j, pub_i, key_j));
    CHECK(mesh_pair_crypto::derive_session_key(priv_i, pub_j, key_i));
    CHECK(std::memcmp(key_i, key_j, 32) == 0);

    uint32_t code_i = 0, code_j = 0;
    CHECK(mesh_pair_crypto::confirmation_code(key_i, &code_i));
    CHECK(mesh_pair_crypto::confirmation_code(key_j, &code_j));
    CHECK(code_i == code_j);
    CHECK(code_i < mesh_pair_crypto::CODE_MODULUS);
  }
  std::printf("PASS two_sides_agree_on_session_key_and_code  (32 independent pairs)\n");
}

// The pre-fix keys: an Ed25519 seed as the "private key" and its Ed25519
// public key. X25519 over them gives the two sides different session keys.
void ed25519_keypair(uint8_t pub[32], uint8_t seed[32]) {
  host_fill(seed, 32);
  EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
  size_t len = 32;
  if (k == nullptr || EVP_PKEY_get_raw_public_key(k, pub, &len) != 1 || len != 32) std::abort();
  EVP_PKEY_free(k);
}

void test_ed25519_keys_do_not_agree() {
  int disagreements = 0;
  for (int round = 0; round < 8; ++round) {
    uint8_t pub_i[32], seed_i[32], pub_j[32], seed_j[32];
    ed25519_keypair(pub_i, seed_i);
    ed25519_keypair(pub_j, seed_j);
    uint8_t key_j[32], key_i[32];
    const bool ok_j = mesh_pair_crypto::derive_session_key(seed_j, pub_i, key_j);
    const bool ok_i = mesh_pair_crypto::derive_session_key(seed_i, pub_j, key_i);
    uint32_t code_i = 0, code_j = 0;
    const bool coded = ok_i && ok_j && mesh_pair_crypto::confirmation_code(key_i, &code_i) &&
                       mesh_pair_crypto::confirmation_code(key_j, &code_j);
    if (!coded || std::memcmp(key_i, key_j, 32) != 0 || code_i != code_j) {
      ++disagreements;
    }
  }
  CHECK(disagreements == 8);
  std::printf("PASS ed25519_keys_do_not_agree  (the pre-fix bug, 8 of 8)\n");
}

void test_code_wire_pin_and_low_order_refusal() {
  // Same derivation as the PlatformIO tree's compute_confirmation_code:
  // test_mesh_pairing pins a 32-byte zero session key to 884555.
  const uint8_t zero_key[32] = {0};
  uint32_t code = 0;
  CHECK(mesh_pair_crypto::confirmation_code(zero_key, &code) && code == 884555u);

  // A low-order peer key (u = 0, u = 1) gives an all-zero X25519 result:
  // refused, and the output wiped.
  uint8_t pub[32], priv[32], key[32];
  CHECK(mesh_pair_crypto::generate_keypair(pub, priv, host_fill));
  const uint8_t u0[32] = {0};
  uint8_t u1[32] = {1};
  std::memset(key, 0xA5, sizeof(key));
  CHECK(!mesh_pair_crypto::derive_session_key(priv, u0, key));
  for (uint8_t b : key) CHECK(b == 0);
  CHECK(!mesh_pair_crypto::derive_session_key(priv, u1, key));
  CHECK(!mesh_pair_crypto::derive_session_key(nullptr, pub, key));
  std::printf("PASS code_wire_pin_and_low_order_refusal\n");
}

std::string hex(const uint8_t* p, size_t n) {
  std::string out;
  char b[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, "%02x", p[i]);
    out += b;
  }
  return out;
}

void test_confirm_hash_is_bound_to_its_senders_role() {
  // The same vectors test_mesh_pairing pins for the PlatformIO tree, each
  // computed independently (openssl dgst over the domain, the key and the
  // code's four little-endian bytes; Python's hashlib).
  using mesh_pair_crypto::ConfirmSender;
  uint8_t zero_key[32] = {0};
  uint8_t seq_key[32];
  for (int i = 0; i < 32; ++i) seq_key[i] = (uint8_t)i;
  uint8_t h[32];
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, zero_key, 884555u, h));
  CHECK(hex(h, 32) == "d34ced10fe3400afaf0819deea176209547d68ad79a298d33c24d4b0646e0f9e");
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::JOINER, zero_key, 884555u, h));
  CHECK(hex(h, 32) == "317ca8743bc47879c88b081e269db104ebbe7a84184c5cce05d9bf31c69b1679");
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, seq_key, 123456u, h));
  CHECK(hex(h, 32) == "dbd355d4806d3e0ee7a382a17e8e2b083f3fa187c5e96574a32c57db23e94e70");
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::JOINER, seq_key, 123456u, h));
  CHECK(hex(h, 32) == "863ce906d7c46fa008ccd77195c05f410256d7efcddb99cae4155f8746d0b6ad");

  // The role-free hash the sketch sent both ways before F94 (the bytes its
  // memcpy of the uint32_t code put after the key) is neither.
  uint8_t in[36] = {0};
  const uint32_t code = 884555u;
  std::memcpy(in + 32, &code, 4);
  CHECK(mesh_crypto::sha256_domain(mesh_pair_crypto::DOMAIN_PAIR_CONFIRM, in, sizeof in, h));
  CHECK(hex(h, 32) == "68b54b5271a8b39345ec536813944be01297d0619b844619a50759c624431ebf");

  CHECK(mesh_pair_crypto::partner_of(ConfirmSender::INITIATOR) == ConfirmSender::JOINER);
  CHECK(mesh_pair_crypto::partner_of(ConfirmSender::JOINER) == ConfirmSender::INITIATOR);

  // None of the three pairing domains is a prefix of another.
  const char* domains[] = {mesh_pair_crypto::DOMAIN_PAIR_CONFIRM,
                           mesh_pair_crypto::DOMAIN_PAIR_CONFIRM_INIT,
                           mesh_pair_crypto::DOMAIN_PAIR_CONFIRM_JOIN};
  for (const char* a : domains) {
    for (const char* b : domains) {
      if (a != b) CHECK(std::strncmp(a, b, std::strlen(a)) != 0);
    }
  }
  std::printf("PASS confirm_hash_is_bound_to_its_senders_role  (d34ced10.../317ca874...)\n");
}

// Sweep F364: a hash that fails gives no code and no CONFIRM. The stand-in
// fails every hash under the domain a case arms; the other domains hash.
void test_a_hash_that_fails_is_refused() {
  using mesh_pair_crypto::ConfirmSender;
  uint8_t key[32];
  host_fill(key, sizeof key);

  // The code: false and 0, not the 000000 a zeroed digest reads as.
  uint32_t code = 123456u;
  pair_fail::arm(mesh_pair_crypto::DOMAIN_PAIR_CONFIRM);
  CHECK(!mesh_pair_crypto::confirmation_code(key, &code));
  CHECK(code == 0 && pair_fail::g_failed == 1);
  // Nor for another key: two sides whose hashes both fail have no code to
  // compare at all (before, each showed 000000 over its own key).
  uint8_t other[32];
  host_fill(other, sizeof other);
  code = 1;
  CHECK(!mesh_pair_crypto::confirmation_code(other, &code) && code == 0);

  // The CONFIRM: false and 32 zero bytes, for the role whose domain fails;
  // the other role's still hashes.
  uint8_t h[32];
  pair_fail::arm(mesh_pair_crypto::DOMAIN_PAIR_CONFIRM_INIT);
  std::memset(h, 0x5C, sizeof h);
  CHECK(!mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, key, 884555u, h));
  for (uint8_t b : h) CHECK(b == 0);
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::JOINER, key, 884555u, h));
  CHECK(pair_fail::g_failed == 1);
  pair_fail::arm(mesh_pair_crypto::DOMAIN_PAIR_CONFIRM_JOIN);
  std::memset(h, 0x5C, sizeof h);
  CHECK(!mesh_pair_crypto::confirm_hash(ConfirmSender::JOINER, key, 884555u, h));
  for (uint8_t b : h) CHECK(b == 0);
  CHECK(mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, key, 884555u, h));

  // And a null argument, which hashes nothing.
  pair_fail::arm(nullptr);
  code = 7;
  CHECK(!mesh_pair_crypto::confirmation_code(nullptr, &code) && code == 0);
  CHECK(!mesh_pair_crypto::confirmation_code(key, nullptr));
  std::memset(h, 0x5C, sizeof h);
  CHECK(!mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, nullptr, 1u, h));
  for (uint8_t b : h) CHECK(b == 0);
  CHECK(!mesh_pair_crypto::confirm_hash(ConfirmSender::INITIATOR, key, 1u, nullptr));
  CHECK(pair_fail::g_failed == 0);
  std::printf("PASS a_hash_that_fails_is_refused  (no code, no CONFIRM)\n");
}

// A refusal as mesh_network.cpp writes it, squeezed: `call` answered false
// logs `line` at ALERT and runs `end`.
std::string refusal(const std::string& call, const std::string& line, const std::string& end) {
  return beacon_source_scan::squeeze("if (!" + call + ") { health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO, \"" +
                                     line + "\"); " + end + " }");
}

void test_mesh_network_calls_this_header() {
  bool ok = false;
  const std::string raw = beacon_source_scan::read_source(MESH_NETWORK_CPP, &ok);
  CHECK(ok);
  const std::string code = beacon_source_scan::strip_comments(raw);
  const std::string sq = beacon_source_scan::squeeze(code);
  using beacon_source_scan::count;
  using beacon_source_scan::function_body;
  using beacon_source_scan::squeeze;

  // Nothing in the file makes Ed25519 keys any more (the device identity key
  // is made in canary_wap.ino, not here).
  CHECK(count(sq, "Ed25519::generatePrivateKey(") == 0);
  CHECK(count(sq, "Ed25519::derivePublicKey(") == 0);

  const std::string disc = squeeze(function_body(code, "handle_pair_discover"));
  const std::string offer = squeeze(function_body(code, "handle_pair_offer"));
  const std::string accept = squeeze(function_body(code, "handle_pair_accept"));
  CHECK(!disc.empty() && !offer.empty() && !accept.empty());

  const std::string gen =
      "if(!mesh_pair_crypto::generate_keypair(g_pairing.ephemeral_pubkey,"
      "g_pairing.ephemeral_privkey,esp_fill_random)){cancel_pairing();return;}";
  CHECK(count(disc, gen) == 1);
  CHECK(count(offer, gen) == 1);

  // A refused derivation cancels; the code comes from confirmation_code().
  CHECK(count(offer, "if(!derive_session_key(g_pairing.ephemeral_privkey,offer->ephemeral_pubkey,"
                     "g_pairing.session_key)){cancel_pairing();return;}") == 1);
  CHECK(count(accept, "if(!derive_session_key(g_pairing.ephemeral_privkey,accept->ephemeral_pubkey,"
                      "g_pairing.session_key)){cancel_pairing();return;}") == 1);
  // F364: a code that did not hash ends the pairing, logged, before the
  // joiner's ACCEPT goes out and before either side shows a code.
  const std::string code_refused =
      refusal("mesh_pair_crypto::confirmation_code(g_pairing.session_key, &g_pairing.confirmation_code)",
              "opera: pairing ended: the code did not hash", "cancel_pairing(); return;");
  CHECK(count(offer, code_refused) == 1);
  CHECK(count(accept, code_refused) == 1);
  CHECK(count(sq, "mesh_pair_crypto::confirmation_code(") == 2);
  CHECK(offer.find(code_refused) < offer.find("send_pair_frame(mac,MSG_PAIR_ACCEPT"));
  CHECK(offer.find(code_refused) < offer.find("g_pairing.code_displayed=true;"));
  CHECK(offer.find(code_refused) < offer.find("g_pairing_callback("));
  CHECK(offer.find(code_refused) > offer.find("derive_session_key("));
  CHECK(accept.find(code_refused) < accept.find("g_pairing.code_displayed=true;"));
  CHECK(accept.find(code_refused) < accept.find("g_pairing_callback("));
  CHECK(accept.find(code_refused) > accept.find("derive_session_key("));
  // In the offer handler the keypair is made BEFORE the session key.
  CHECK(offer.find(gen) < offer.find("derive_session_key("));

  // derive_session_key is the header's, and there is no second copy.
  const std::string dsk = squeeze(function_body(code, "derive_session_key"));
  CHECK(count(dsk, "returnmesh_pair_crypto::derive_session_key(local_priv,peer_pub,key_out);") == 1);
  CHECK(count(sq, "mbedtls_hkdf(") == 0);
  CHECK(count(sq, "Curve25519::eval(") == 0);

  // F94: the CONFIRM sent is this side's role's, the one checked its
  // partner's, and no other CONFIRM hash is left in the file.
  const std::string hpc = squeeze(function_body(code, "handle_pair_confirm"));
  const std::string cp = squeeze(function_body(code, "confirm_pairing"));
  CHECK(!hpc.empty() && !cp.empty());
  // F364: each refuses a hash that did not compute, logged: the check
  // before any compare, the send before the CONFIRM goes out (confirm_pairing
  // ends the pairing as a refused partner's ends, and answers false).
  const std::string check_refused =
      refusal("mesh_pair_crypto::confirm_hash(mesh_pair_crypto::partner_of(confirm_sender(g_pairing.role)), "
              "g_pairing.session_key, g_pairing.confirmation_code, expected_hash)",
              "opera: pairing ended: the partner's CONFIRM could not be checked",
              "cancel_pairing(); return;");
  const std::string send_refused =
      refusal("mesh_pair_crypto::confirm_hash(confirm_sender(g_pairing.role), g_pairing.session_key, "
              "g_pairing.confirmation_code, confirm.confirmation_hash)",
              "opera: pairing ended: this side's CONFIRM did not hash",
              "end_refused_pairing(); return false;");
  CHECK(count(hpc, check_refused) == 1);
  CHECK(hpc.find(check_refused) < hpc.find("memcmp(confirm->confirmation_hash"));
  CHECK(count(cp, send_refused) == 1);
  CHECK(cp.find(send_refused) < cp.find("send_pair_frame("));
  CHECK(count(sq, "mesh_pair_crypto::confirm_hash(") == 2);
  CHECK(count(sq, "DOMAIN_PAIR_CONFIRM") == 0);
  const std::string role = squeeze(function_body(code, "confirm_sender"));
  CHECK(count(role, "returnrole==PAIR_ROLE_INITIATOR?mesh_pair_crypto::ConfirmSender::INITIATOR"
                    ":mesh_pair_crypto::ConfirmSender::JOINER;") == 1);
  std::printf("PASS mesh_network_calls_this_header\n");
}

}  // namespace

int main() {
  test_rfc7748_alice_bob_through_the_stand_in();
  test_generate_keypair_is_clamped_x25519();
  test_two_sides_agree_on_session_key_and_code();
  test_ed25519_keys_do_not_agree();
  test_code_wire_pin_and_low_order_refusal();
  test_confirm_hash_is_bound_to_its_senders_role();
  test_a_hash_that_fails_is_refused();
  test_mesh_network_calls_this_header();
  std::printf("\nALL mesh pair-crypto (canary-wap) tests PASSED (%d checks)\n", g_checks);
  return 0;
}
