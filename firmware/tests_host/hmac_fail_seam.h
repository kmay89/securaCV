// hmac_fail_seam.h — the HMAC primitive under common/network/fleet_key.h,
// replaced by one the *_hmac_fail suites can make fail (sweep F350 review).
//
// fleet_key.h has two branches, and the device ships the first:
//   - with ARDUINO defined it calls mbedTLS, mbedtls_md_info_from_type() and
//     mbedtls_md_hmac(); the Makefile builds that branch on the host with
//     -DARDUINO -I stubs/mbedtls_md (mbedTLS 2.28's declarations), and this
//     header defines the two functions;
//   - without it, it calls OpenSSL's one-shot HMAC(), and this header defines
//     HMAC() itself (the program's own definition wins over libcrypto's at
//     link time).
// Each suite is built both ways, so a defect in either branch's failure test
// fails a host suite, not only a device that cannot allocate.
//
// seam::g_mode picks what the primitive does. Pass is a real HMAC-SHA256 in
// both builds (OpenSSL's HMAC_CTX, which never calls HMAC()), so a suite can
// make a frame or a key with the real primitive and then fail the next call.
// The failure modes are the ones each library can report:
//   device: AllocFailed (mbedtls_md_hmac() returns MBEDTLS_ERR_MD_ALLOC_FAILED,
//           what mbedtls_md_setup() answers when it cannot allocate) and
//           NoInfo (mbedtls_md_info_from_type() finds no SHA-256);
//   host:   NoResult (HMAC() returns NULL) and ShortResult (it returns 20
//           bytes, what a SHA-1 HMAC would).
// A failing stand-in scribbles 0xA5 over the output first, so a zeroed output
// after a failure is the checked form's own memset, not the stand-in's.
// seam::g_calls counts every call that reached the primitive, so a pass is the
// refusal's, not a real HMAC's. For a computation that makes several HMACs
// (the HKDF's extract and expand rounds), seam::g_pass_first lets that many
// operations run for real before g_mode applies, and seam::g_fail_count, when
// it is not -1, lets only that many fail before the rest run for real again.
//
// One TU per binary: this defines non-inline functions with C linkage.
#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(ARDUINO)
#include "mbedtls/md.h"  // stubs/mbedtls_md
#endif

namespace seam {

enum class Mode {
  Pass,
#if defined(ARDUINO)
  AllocFailed,
  NoInfo,
#else
  NoResult,
  ShortResult,
#endif
};

#if defined(ARDUINO)
static constexpr const char* kBranch = "mbedTLS (device) branch";
static constexpr Mode kFailures[] = {Mode::AllocFailed, Mode::NoInfo};
#else
static constexpr const char* kBranch = "OpenSSL (host) branch";
static constexpr Mode kFailures[] = {Mode::NoResult, Mode::ShortResult};
#endif

inline Mode g_mode = Mode::Pass;
inline int g_calls = 0;
inline int g_pass_first = 0;
inline int g_fail_count = -1;

// What the next HMAC operation does: Pass while g_pass_first lasts, then
// g_mode, for g_fail_count operations when that is not -1, then Pass again.
inline Mode next_operation() {
  if (g_pass_first > 0) {
    --g_pass_first;
    return Mode::Pass;
  }
  if (g_fail_count == 0) return Mode::Pass;
  if (g_fail_count > 0) --g_fail_count;
  return g_mode;
}

inline const char* name_of(Mode m) {
  switch (m) {
    case Mode::Pass: return "pass";
#if defined(ARDUINO)
    case Mode::AllocFailed: return "mbedtls_md_hmac: MBEDTLS_ERR_MD_ALLOC_FAILED";
    case Mode::NoInfo: return "mbedtls_md_info_from_type: no SHA-256";
#else
    case Mode::NoResult: return "HMAC(): no result";
    case Mode::ShortResult: return "HMAC(): a 20-byte result";
#endif
  }
  return "?";
}

// A real HMAC-SHA256 that does not go through HMAC(), which the host build
// replaces. True when it ran and wrote 32 bytes.
inline bool real_hmac_sha256(const void* key, size_t klen, const unsigned char* msg, size_t mlen,
                             unsigned char out[32]) {
  HMAC_CTX* c = HMAC_CTX_new();
  if (c == nullptr) return false;
  unsigned int olen = 0;
  const bool ok = HMAC_Init_ex(c, key, (int)klen, EVP_sha256(), nullptr) == 1 &&
                  HMAC_Update(c, msg, mlen) == 1 && HMAC_Final(c, out, &olen) == 1 && olen == 32;
  HMAC_CTX_free(c);
  return ok;
}

}  // namespace seam

#if defined(ARDUINO)

// The opaque type mbedTLS keeps private, completed here so the stand-in can
// hand out one real-looking info and recognize it.
struct mbedtls_md_info_t {
  mbedtls_md_type_t type;
};
static const mbedtls_md_info_t kSeamSha256Info = {MBEDTLS_MD_SHA256};

// fleet_key.h looks the info up once per HMAC, before mbedtls_md_hmac(), so the
// lookup is where an operation's mode is decided.
static seam::Mode g_seam_current = seam::Mode::Pass;

extern "C" const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t md_type) {
  ++seam::g_calls;
  g_seam_current = seam::next_operation();
  if (g_seam_current == seam::Mode::NoInfo || md_type != MBEDTLS_MD_SHA256) return nullptr;
  return &kSeamSha256Info;
}

extern "C" int mbedtls_md_hmac(const mbedtls_md_info_t* md_info, const unsigned char* key,
                               size_t keylen, const unsigned char* input, size_t ilen,
                               unsigned char* output) {
  ++seam::g_calls;
  // What mbedTLS 2.28 answers a null info (md.h: MBEDTLS_ERR_MD_BAD_INPUT_DATA
  // on parameter-verification failure).
  if (md_info != &kSeamSha256Info) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  if (g_seam_current == seam::Mode::AllocFailed) {
    std::memset(output, 0xA5, 32);
    return MBEDTLS_ERR_MD_ALLOC_FAILED;
  }
  return seam::real_hmac_sha256(key, keylen, input, ilen, output) ? 0
                                                                   : MBEDTLS_ERR_MD_BAD_INPUT_DATA;
}

#else

extern "C" unsigned char* HMAC(const EVP_MD* /*evp_md*/, const void* key, int key_len,
                               const unsigned char* data, size_t data_len, unsigned char* md,
                               unsigned int* md_len) {
  ++seam::g_calls;
  const seam::Mode m = seam::next_operation();
  if (m == seam::Mode::Pass) {
    if (!seam::real_hmac_sha256(key, (size_t)key_len, data, data_len, md)) return nullptr;
    if (md_len != nullptr) *md_len = 32;
    return md;
  }
  std::memset(md, 0xA5, 32);
  if (m == seam::Mode::NoResult) return nullptr;
  if (md_len != nullptr) *md_len = 20;
  return md;
}

#endif
