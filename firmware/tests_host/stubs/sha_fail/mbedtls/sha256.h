/* A fail-injectable host stand-in for mbedTLS's streaming SHA-256, for
 * test_sha_fail_flagship.cpp (sweep F455): a real SHA-256 over OpenSSL, or
 * a failure at the step the test names, once.
 *
 * Two spellings, picked by SHA_STUB_MBEDTLS_VERSION (mbedtls/version.h
 * beside this file), as the two cores ship them:
 *   - mbedTLS 2.28 (Arduino core 2.0.17): the _ret names answer an int, and
 *     the plain names return void (they call the _ret name and drop its
 *     answer, as 2.28's do), so code that calls a plain name has no answer
 *     to read. 2.28's sha256.h does not define MBEDTLS_VERSION_NUMBER:
 *     only version.h does, so this file does not include it either.
 *   - mbedTLS 3.6 (Arduino core 3.x): the _ret names are gone (a call to
 *     one does not compile) and the plain names answer an int. Every 3.x
 *     header reaches build_info.h, which defines MBEDTLS_VERSION_NUMBER, so
 *     this file includes version.h in that spelling.
 *
 * Each mbedtls_sha256_init() starts a hash, numbered from the last
 * sha_stub::arm(). The hash arm() names fails at its step, once: the start,
 * the update it names (counting from 0) or the finish, which writes 0xA5
 * over the output first (so the zeros a caller sees are its own). Every
 * other step runs as it would have, over what it was fed: mbedTLS does not
 * promise that a step after a failed one fails, and IDF's DMA port keeps no
 * error in the context (the canary-wap's F364 review). So code that drops a
 * failed start's answer gets the right digest, answered 0, and code that
 * drops a failed update's answer goes on to a finish over input missing that
 * update: either way the helper's answer and its output give it away. A step
 * called on a context after its failure is counted (steps_after_failure).
 *
 * Host only; it proves nothing about the hardware engine. */
#pragma once

#include <stddef.h>
#include <string.h>

#include <openssl/evp.h>

#ifndef SHA_STUB_MBEDTLS_VERSION
#error "build with -DSHA_STUB_MBEDTLS_VERSION=0x021C0700 (mbedTLS 2.28) or 0x03060300 (mbedTLS 3.6)"
#endif
#if SHA_STUB_MBEDTLS_VERSION >= 0x03000000
#include "mbedtls/version.h"
#endif

typedef struct mbedtls_sha256_context {
  EVP_MD_CTX* md;
  int index;    // which hash since arm()
  int updates;  // updates called on this context
  bool failed;  // a step on this context has answered an error
} mbedtls_sha256_context;

namespace sha_stub {
enum class Step { None, Starts, Update, Finish };
// mbedTLS's MBEDTLS_ERR_SHA256_HW_ACCEL_FAILED (2.x) / a port's error.
static const int kFailed = -0x0037;

inline Step g_fail = Step::None;
inline int g_fail_hash = 0;
inline int g_fail_update = 0;
inline bool g_fired = false;
inline int g_hashes = 0;               // inits since arm()
inline int g_inits = 0;
inline int g_frees = 0;
inline int g_live = 0;                 // contexts inited and not freed, ever
inline int g_steps_after_failure = 0;  // steps called on a context after its failure

inline void arm(Step fail, int hash = 0, int update = 0) {
  g_fail = fail;
  g_fail_hash = hash;
  g_fail_update = update;
  g_fired = false;
  g_hashes = g_inits = g_frees = 0;
  g_steps_after_failure = 0;
}
inline void disarm() { arm(Step::None); }

inline bool failing_here(mbedtls_sha256_context* ctx, Step step) {
  if (g_fail != step || ctx->index != g_fail_hash) return false;
  if (step == Step::Update && ctx->updates != g_fail_update) return false;
  g_fired = true;
  return true;
}
inline void note_step(mbedtls_sha256_context* ctx) {
  if (ctx->failed) g_steps_after_failure++;
}
}  // namespace sha_stub

inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  ++sha_stub::g_inits;
  ++sha_stub::g_live;
  ctx->md = EVP_MD_CTX_new();
  ctx->index = sha_stub::g_hashes++;
  ctx->updates = 0;
  ctx->failed = false;
}

inline void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
  ++sha_stub::g_frees;
  if (ctx == nullptr || ctx->md == nullptr) return;
  EVP_MD_CTX_free(ctx->md);
  ctx->md = nullptr;
  --sha_stub::g_live;
}

// The steps with an answer: the _ret names on 2.x, the plain names on 3.x.
inline int sha_stub_starts(mbedtls_sha256_context* ctx, int is224) {
  sha_stub::note_step(ctx);
  if (is224 != 0) return sha_stub::kFailed;  // the firmware asks for SHA-256 only
  // A failed start still leaves a started state (an error answered after
  // the engine was set up), so the steps after it run.
  const bool fail = sha_stub::failing_here(ctx, sha_stub::Step::Starts);
  const bool ok = EVP_DigestInit_ex(ctx->md, EVP_sha256(), nullptr) == 1;
  if (fail || !ok) {
    ctx->failed = true;
    return sha_stub::kFailed;
  }
  return 0;
}

inline int sha_stub_update(mbedtls_sha256_context* ctx, const unsigned char* in, size_t n) {
  sha_stub::note_step(ctx);
  const bool fail = sha_stub::failing_here(ctx, sha_stub::Step::Update);
  ctx->updates++;
  if (fail) {  // this update's bytes never reach the hash
    ctx->failed = true;
    return sha_stub::kFailed;
  }
  if (n == 0) return 0;
  return EVP_DigestUpdate(ctx->md, in, n) == 1 ? 0 : sha_stub::kFailed;
}

inline int sha_stub_finish(mbedtls_sha256_context* ctx, unsigned char* out) {
  sha_stub::note_step(ctx);
  if (sha_stub::failing_here(ctx, sha_stub::Step::Finish)) {
    memset(out, 0xA5, 32);
    ctx->failed = true;
    return sha_stub::kFailed;
  }
  unsigned int olen = 0;
  return EVP_DigestFinal_ex(ctx->md, out, &olen) == 1 && olen == 32 ? 0 : sha_stub::kFailed;
}

#if SHA_STUB_MBEDTLS_VERSION >= 0x03000000
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224) {
  return sha_stub_starts(ctx, is224);
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input,
                                 size_t ilen) {
  return sha_stub_update(ctx, input, ilen);
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* output) {
  return sha_stub_finish(ctx, output);
}
#else
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* ctx, int is224) {
  return sha_stub_starts(ctx, is224);
}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* ctx, const unsigned char* input,
                                     size_t ilen) {
  return sha_stub_update(ctx, input, ilen);
}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* ctx, unsigned char output[32]) {
  return sha_stub_finish(ctx, output);
}
// 2.28's deprecated plain names: void, the answer dropped.
inline void mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224) {
  (void)mbedtls_sha256_starts_ret(ctx, is224);
}
inline void mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input,
                                  size_t ilen) {
  (void)mbedtls_sha256_update_ret(ctx, input, ilen);
}
inline void mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32]) {
  (void)mbedtls_sha256_finish_ret(ctx, output);
}
#endif
