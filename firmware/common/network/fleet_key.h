/**
 * @file fleet_key.h
 * @brief The household fleet key — what makes two Canaries "paired".
 *
 * The fleet-link beacon is unsigned presence ("trust never rises above
 * presence"), and nothing below the WAP's Opera mesh authenticates a peer.
 * The program brief asks that only paired nodes accept each other over the
 * peer protocol (common/network/peer_protocol.h). This header is the key
 * that protocol tags every frame with, and how a node gets it:
 *
 *   BOOTSTRAP — derived from the household Wi-Fi the owner handed the node:
 *     key = HKDF-SHA256(salt = "securacv:fleet-key:v1",
 *                       ikm  = ssid || 0x00 || password,
 *                       info = "peer")
 *   Every node that was handed this network by its owner derives the same
 *   32 bytes; a stranger on the street, or a guest on the guest network,
 *   does not. "Paired" therefore means "provisioned onto this household's
 *   network by someone who knew its password" — exactly the trust the
 *   onboarding step already establishes, and nothing stronger. An OPEN
 *   network carries no secret, so derivation REFUSES (false): the peer
 *   protocol then stays off with a logged warning rather than running
 *   unauthenticated (the brief's "explicit degradation").
 *
 *   ROTATION — the hub may hand every node a replacement key (32 random
 *   bytes over the broker, retained, on the node's own command topic, which
 *   only the shared broker login can publish to) and nodes store it under
 *   NVS `fleet_key`; the stored key wins over the derived one. The command
 *   path, its limits, and what an attacker with the broker login can do are
 *   written down in docs/TRUST_MODEL.md.
 *
 * Primitives: HMAC-SHA256 (mbedTLS on device, OpenSSL on the host — the
 * device_pseudonym.h split) and RFC 5869 HKDF built on it here, so one
 * implementation runs on the chip and in `firmware/tests_host/
 * test_fleet_key.cpp` against the RFC vectors.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ARDUINO)
  #include "mbedtls/md.h"
#else
  #include <openssl/hmac.h>
  #include <openssl/evp.h>
#endif

namespace canary {
namespace net {
namespace fleet_key {

static constexpr size_t KEY_BYTES = 32;
static constexpr size_t TAG_BYTES = 16;
static constexpr size_t HASH_BYTES = 32;
static constexpr const char* NVS_KEY = "fleet_key";
static constexpr const char* SALT = "securacv:fleet-key:v1";
static constexpr const char* INFO_PEER = "peer";

/// HMAC-SHA256 that says whether it ran. On a failure (on the device,
/// mbedtls_md_hmac() reports one when it cannot allocate its context) `out`
/// is zeroed and the answer is false. A caller that turns the output into a
/// secret, a tag or an id must take this form and refuse on false: 32 zero
/// bytes are the same on every unit, so a key rendered from them is one key
/// for all of them (ap_password.h, sweep F350), and a tag compared against
/// them accepts a frame that carries zeros. Every caller in this header does.
/// tests_host/test_ap_password_hmac_fail.cpp and test_fleet_key_hmac_fail.cpp
/// fail both branches below.
inline bool hmac_sha256_checked(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen,
                                uint8_t out[HASH_BYTES]) {
#if defined(ARDUINO)
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr || mbedtls_md_hmac(md, key, klen, msg, mlen, out) != 0) {
    memset(out, 0, HASH_BYTES);
    return false;
  }
  return true;
#else
  unsigned int olen = 0;
  if (HMAC(EVP_sha256(), key, (int)klen, msg, mlen, out, &olen) == nullptr || olen != HASH_BYTES) {
    memset(out, 0, HASH_BYTES);
    return false;
  }
  return true;
#endif
}

/// The unchecked form: the same HMAC, zeroed output on a failure, nothing
/// reported. Only for a caller with nothing secret riding on the output (the
/// RFC 4231 vectors in test_fleet_key.cpp); nothing in this header calls it.
inline void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen,
                        uint8_t out[HASH_BYTES]) {
  (void)hmac_sha256_checked(key, klen, msg, mlen, out);
}

namespace detail {
/// Zero `n` bytes through a volatile pointer, so the store survives DCE.
inline void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < n; i++) v[i] = 0;
}
}  // namespace detail

/// RFC 5869 HKDF-SHA256 (extract + expand). `olen` ≤ 255 * 32. False, with
/// `okm` wiped, when any of its HMACs fails: an all-zero key reported as
/// derived would be the same key on every node, and anyone can tag frames
/// under it.
inline bool hkdf_sha256(const uint8_t* salt, size_t slen, const uint8_t* ikm, size_t ilen,
                        const uint8_t* info, size_t infolen, uint8_t* okm, size_t olen) {
  if (okm == nullptr || olen == 0 || olen > 255 * HASH_BYTES) return false;
  if (infolen > 255) return false;
  uint8_t prk[HASH_BYTES];
  static const uint8_t kZeroSalt[HASH_BYTES] = {0};
  bool ran = hmac_sha256_checked(slen ? salt : kZeroSalt, slen ? slen : HASH_BYTES, ikm, ilen, prk);
  uint8_t t[HASH_BYTES] = {0};  // T(0) is empty: the first round copies tlen == 0 bytes of it
  size_t tlen = 0;
  size_t done = 0;
  uint8_t counter = 1;
  uint8_t buf[HASH_BYTES + 255 + 1];
  while (ran && done < olen) {
    size_t o = 0;
    memcpy(buf + o, t, tlen); o += tlen;
    if (info && infolen) { memcpy(buf + o, info, infolen); o += infolen; }
    buf[o++] = counter++;
    ran = hmac_sha256_checked(prk, HASH_BYTES, buf, o, t);
    tlen = HASH_BYTES;
    const size_t take = (olen - done < HASH_BYTES) ? olen - done : HASH_BYTES;
    memcpy(okm + done, t, take);
    done += take;
  }
  detail::wipe(prk, sizeof(prk));
  detail::wipe(t, sizeof(t));
  detail::wipe(buf, sizeof(buf));
  if (!ran) detail::wipe(okm, olen);
  return ran;
}

/**
 * @brief The bootstrap key from the household Wi-Fi. False on an open network
 * (no secret to derive from), a bad SSID or a failed HMAC (`out` is then
 * wiped) — the caller logs and leaves the peer protocol off.
 */
inline bool derive_from_wifi(const char* ssid, const char* pass, uint8_t out[KEY_BYTES]) {
  if (out == nullptr || ssid == nullptr || ssid[0] == '\0') return false;
  if (pass == nullptr || pass[0] == '\0') return false;
  const size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl > 32 || pl > 64) return false;
  uint8_t ikm[32 + 1 + 64];
  memcpy(ikm, ssid, sl);
  ikm[sl] = 0;
  memcpy(ikm + sl + 1, pass, pl);
  return hkdf_sha256((const uint8_t*)SALT, strlen(SALT), ikm, sl + 1 + pl,
                     (const uint8_t*)INFO_PEER, strlen(INFO_PEER), out, KEY_BYTES);
}

/// Truncated HMAC-SHA256 over `msg` — the tag every peer frame carries.
/// False, with `out` zeroed, when the HMAC did not run: a zero tag is no tag,
/// so peer::encode() sends nothing and peer::decode() answers BadTag rather
/// than compare a frame's tag against it.
inline bool tag(const uint8_t key[KEY_BYTES], const uint8_t* msg, size_t n, uint8_t out[TAG_BYTES]) {
  uint8_t full[HASH_BYTES];
  const bool ran = hmac_sha256_checked(key, KEY_BYTES, msg, n, full);
  memcpy(out, full, TAG_BYTES);
  return ran;
}

/// Constant-time compare.
inline bool tag_equal(const uint8_t a[TAG_BYTES], const uint8_t b[TAG_BYTES]) {
  uint8_t d = 0;
  for (size_t i = 0; i < TAG_BYTES; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

/// A short public id for logs and status — eight hex chars of SHA-256-HMAC
/// of the key under a fixed label, never the key itself. False, with `out`
/// set to "", when the HMAC did not run, rather than the id of a zero hash.
inline bool key_id_hex(const uint8_t key[KEY_BYTES], char out[9]) {
  static const char kLabel[] = "securacv:fleet-key:id";
  uint8_t h[HASH_BYTES];
  if (!hmac_sha256_checked(key, KEY_BYTES, (const uint8_t*)kLabel, sizeof(kLabel) - 1, h)) {
    out[0] = '\0';
    return false;
  }
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    out[2 * i] = hex[h[i] >> 4];
    out[2 * i + 1] = hex[h[i] & 0xf];
  }
  out[8] = '\0';
  return true;
}

}  // namespace fleet_key
}  // namespace net
}  // namespace canary
