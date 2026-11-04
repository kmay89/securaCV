/* Host stand-in for mbedtls' SHA-256 — OpenSSL's SHA-256: the context API
 * (init, starts, update, finish, free) and the one-shot mbedtls_sha256().
 *
 * A fail knob (sweep F364, test_wap_sha_fail_modules.cpp): a test that calls
 * stub_sha256::arm() names a step, and the hash it names (counted from 0
 * since arm(): each init() or one-shot call begins one) fails at that step,
 * once, answering MBEDTLS_ERR_SHA256_HW_ACCEL_FAILED. A failed start still
 * sets the hash up, so the steps after it run; a failed update feeds
 * nothing; a failed finish or one-shot leaves its output as the test chose
 * (0xA5 bytes, untouched, or the true digest of what it was fed — the last
 * is what a caller that ignores the answer would read when its stack still
 * held a digest). Every other step runs as it would have: mbedTLS does not
 * promise that a step after a failed one fails, and IDF's DMA port keeps no
 * error in the context. Armed, the stub also counts the contexts inited and
 * not freed (live), so a test can see each one freed on every path.
 *
 * Not armed — the default, and what every other suite that includes this
 * header runs on — the knob counts nothing and fails nothing: each step
 * answers 0 (or -1 if OpenSSL itself fails) over exactly what it is fed,
 * as before the knob. */
#ifndef STUB_MESH_NET_MBEDTLS_SHA256_H
#define STUB_MESH_NET_MBEDTLS_SHA256_H

#include <stddef.h>
#include <string.h>

#include <openssl/evp.h>

namespace stub_sha256 {
enum class Step { None, Starts, Update, Finish, OneShot };
enum class Out { Scribble, Untouched, Digest };  // what a failed finish or one-shot leaves
const int kFailed = -0x0037;  // mbedTLS's MBEDTLS_ERR_SHA256_HW_ACCEL_FAILED

inline bool armed = false;
inline Step fail_step = Step::None;
inline int fail_hash = 0;
inline int fail_update = 0;
inline Out fail_out = Out::Scribble;
inline int hashes = 0;   // hashes begun since arm()
inline int live = 0;     // contexts inited since arm() and not freed
inline bool fired = false;  // the armed step was reached

/* Count from here; fail `step` of hash `hash` (its update `update`, from 0),
 * once. arm() with Step::None counts and fails nothing. */
inline void arm(Step step = Step::None, int hash = 0, int update = 0, Out out = Out::Scribble) {
  armed = true;
  fail_step = step;
  fail_hash = hash;
  fail_update = update;
  fail_out = out;
  hashes = live = 0;
  fired = false;
}
inline void disarm() {
  arm();
  armed = false;
}
/* True once, at the armed step of the armed hash. */
inline bool failing_here(Step step, int index, int update = 0) {
  if (!armed || fired || fail_step != step || index != fail_hash) return false;
  if (step == Step::Update && update != fail_update) return false;
  fired = true;
  return true;
}
}  // namespace stub_sha256

typedef struct {
  EVP_MD_CTX* md;
  int index;    // which hash since arm(); -1 when the knob is not armed
  int updates;  // updates so far
} mbedtls_sha256_context;

inline void mbedtls_sha256_init(mbedtls_sha256_context* c) {
  c->md = EVP_MD_CTX_new();
  c->index = -1;
  c->updates = 0;
  if (stub_sha256::armed) {
    c->index = stub_sha256::hashes++;
    ++stub_sha256::live;
  }
}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int /*is224*/) {
  const bool fail = stub_sha256::failing_here(stub_sha256::Step::Starts, c->index);
  const bool ok = EVP_DigestInit_ex(c->md, EVP_sha256(), nullptr) == 1;
  if (fail) return stub_sha256::kFailed;
  return ok ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const unsigned char* d, size_t n) {
  const bool fail = stub_sha256::failing_here(stub_sha256::Step::Update, c->index, c->updates);
  c->updates++;
  if (fail) return stub_sha256::kFailed;  // these bytes never reach the hash
  return EVP_DigestUpdate(c->md, d, n) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, unsigned char out[32]) {
  unsigned int n = 32;
  if (stub_sha256::failing_here(stub_sha256::Step::Finish, c->index)) {
    if (stub_sha256::fail_out == stub_sha256::Out::Scribble) memset(out, 0xA5, 32);
    if (stub_sha256::fail_out == stub_sha256::Out::Digest) EVP_DigestFinal_ex(c->md, out, &n);
    return stub_sha256::kFailed;
  }
  return EVP_DigestFinal_ex(c->md, out, &n) == 1 ? 0 : -1;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c) {
  if (c->index >= 0 && c->md != nullptr) --stub_sha256::live;
  EVP_MD_CTX_free(c->md);
  c->md = nullptr;
}

/* The one-shot: SHA-256 only (is224 must be 0, as every caller passes). */
inline int mbedtls_sha256(const unsigned char* in, size_t n, unsigned char out[32], int is224) {
  if (is224 != 0) return -1;
  const int index = stub_sha256::armed ? stub_sha256::hashes++ : -1;
  unsigned int olen = 32;
  if (stub_sha256::failing_here(stub_sha256::Step::OneShot, index)) {
    if (stub_sha256::fail_out == stub_sha256::Out::Scribble) memset(out, 0xA5, 32);
    if (stub_sha256::fail_out == stub_sha256::Out::Digest) {
      EVP_Digest(in, n, out, &olen, EVP_sha256(), nullptr);
    }
    return stub_sha256::kFailed;
  }
  return EVP_Digest(in, n, out, &olen, EVP_sha256(), nullptr) == 1 && olen == 32 ? 0 : -1;
}

#endif
