/*
 * SecuraCV canary-wap — Opera pairing key agreement
 *
 * CRYPTO (F33 part 2 — crypto review, maintainer to confirm).
 *
 * THE BUG THIS FIXES: the pairing handlers made their ephemeral keys with
 * Ed25519::generatePrivateKey() + Ed25519::derivePublicKey() and then ran
 * Curve25519::eval() — X25519 — over them. An Ed25519 public key is an
 * Edwards-curve point derived from SHA-512(seed); it is not the Montgomery
 * u-coordinate of the seed times the base point. So the joiner's
 * X25519(seed_j, edpub_i) and the initiator's X25519(seed_i, edpub_j) are
 * two unrelated numbers, the two session keys differ, and the 6-digit codes
 * the user is asked to compare could never match: WAP-to-WAP pairing could
 * not complete on a device. (The PlatformIO tree had the same bug; its host
 * test hid it behind an X25519 shim that agreed whatever the keys were.)
 *
 * THE FIX: a clamped Curve25519 keypair from the library the sketch already
 * uses (rweather/Crypto): 32 bytes from the hardware RNG, clamped per
 * RFC 7748 §5 (Curve25519::eval takes the scalar as given — it does not
 * clamp), pub = Curve25519::eval(priv, basepoint). This is the sequence
 * rweather's own Curve25519::dh1() runs, but it draws from esp_fill_random()
 * rather than the library's RNG object, which this sketch never seeds.
 *
 * What moved here from mesh_network.cpp, unchanged on the wire: the
 * session-key derivation (X25519, then HKDF-SHA256 with the
 * "securacv:mesh:session:v0" info string, no salt) and the 6-digit
 * confirmation code (SHA-256("securacv:pair:confirm:v0" || session_key),
 * top 24 bits mod 10^6). One addition: an all-zero X25519 result (the peer
 * sent a low-order point) is refused, as the PlatformIO tree and this
 * sketch's Beacon cosign path already refuse it.
 *
 * NOT fixed here, and not claimed: the two trees still do not pair with
 * EACH OTHER. Besides their different frame numbering (mesh_pair_frame.h),
 * this tree runs the X25519 output through HKDF while the PlatformIO tree
 * (and spec §5.3) uses it directly as the pairing session key, so a mixed
 * pair would still show two different codes. And the AUTH path's
 * derive_session_key(g_device_privkey, peer->pubkey) in mesh_network.cpp
 * still runs X25519 over the long-term Ed25519 identity keys — the same
 * class of bug, in the per-peer session keys MSG_OPERA_REKEY encrypts under.
 *
 * Arduino-free: it needs only <Curve25519.h>, <mbedtls/md.h>,
 * <mbedtls/hkdf.h> and mesh_crypto.h's sha256_domain, so the host test
 * (tests_host/test_mesh_pair_crypto.cpp) compiles THIS code against real
 * implementations of those four (OpenSSL's X25519, HKDF and SHA-256) — no
 * mock of the key exchange — and pins that mesh_network.cpp calls it.
 */

#ifndef CANARY_WAP_MESH_PAIR_CRYPTO_H
#define CANARY_WAP_MESH_PAIR_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <Curve25519.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>

#include "mesh_crypto.h"

namespace mesh_pair_crypto {

constexpr size_t   KEY_LEN      = 32;
constexpr uint32_t CODE_MODULUS = 1000000;

/* Wire strings, byte-identical to what the sketch always used. */
constexpr const char DOMAIN_SESSION[]      = "securacv:mesh:session:v0";
constexpr const char DOMAIN_PAIR_CONFIRM[] = "securacv:pair:confirm:v0";

/* esp_fill_random() on the device. */
using FillRandomFn = void (*)(void* buf, size_t len);

/* Zeroize in a way the optimizer cannot elide (a plain memset on a dying
 * local is a dead store the compiler may delete). */
inline void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

/* RFC 7748 §5: clear the low three bits, clear bit 255, set bit 254. */
inline void clamp_scalar(uint8_t k[KEY_LEN]) {
  k[0]  &= 0xF8;
  k[31]  = (uint8_t)((k[31] & 0x7F) | 0x40);
}

/* A fresh X25519 keypair: priv = clamp(random), pub = priv * basepoint.
 * Returns false (priv wiped) on a null argument or when eval() refuses four
 * times running — for the base point it never does. */
inline bool generate_keypair(uint8_t pub[KEY_LEN], uint8_t priv[KEY_LEN],
                             FillRandomFn fill) {
  if (pub == nullptr || priv == nullptr || fill == nullptr) return false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    fill(priv, KEY_LEN);
    clamp_scalar(priv);
    if (Curve25519::eval(pub, priv, nullptr)) return true;
  }
  secure_wipe(priv, KEY_LEN);
  return false;
}

inline bool all_zero(const uint8_t* p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) acc |= p[i];
  return acc == 0;
}

/* session_key = HKDF-SHA256(salt = none, ikm = X25519(local_priv, peer_pub),
 * info = DOMAIN_SESSION, 32 bytes). False on a refused or all-zero X25519
 * result or an HKDF error; key_out is wiped then. */
inline bool derive_session_key(const uint8_t local_priv[KEY_LEN],
                               const uint8_t peer_pub[KEY_LEN],
                               uint8_t key_out[KEY_LEN]) {
  if (local_priv == nullptr || peer_pub == nullptr || key_out == nullptr) return false;
  uint8_t shared[KEY_LEN];
  if (!Curve25519::eval(shared, local_priv, peer_pub) || all_zero(shared, KEY_LEN)) {
    secure_wipe(shared, sizeof(shared));
    secure_wipe(key_out, KEY_LEN);
    return false;
  }
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const int ret = mbedtls_hkdf(md,
                               nullptr, 0,   /* no salt */
                               shared, sizeof(shared),
                               (const uint8_t*)DOMAIN_SESSION, strlen(DOMAIN_SESSION),
                               key_out, KEY_LEN);
  secure_wipe(shared, sizeof(shared));
  if (ret != 0) {
    secure_wipe(key_out, KEY_LEN);
    return false;
  }
  return true;
}

/* The 6-digit code both screens show: top 24 bits of
 * SHA-256(DOMAIN_PAIR_CONFIRM || session_key), mod 10^6. */
inline uint32_t confirmation_code(const uint8_t session_key[KEY_LEN]) {
  uint8_t h[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain(DOMAIN_PAIR_CONFIRM, session_key, KEY_LEN, h);
  const uint32_t top24 = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | (uint32_t)h[2];
  return top24 % CODE_MODULUS;
}

}  /* namespace mesh_pair_crypto */

#endif  /* CANARY_WAP_MESH_PAIR_CRYPTO_H */
