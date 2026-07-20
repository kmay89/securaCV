// End-to-end host test for the drop-file update pipeline: generate a real
// Ed25519 release key (OpenSSL), sign a manifest exactly the way
// firmware/scripts/ota_release.py does (canonical field string + image
// signature over size_LE32||sha256), then prove evidence_update::verify
// accepts the genuine pair and rejects every tampered variant.
//
// Links: evidence_update_verify.cpp + securacv_ota.cpp (pure subset via
// -DSECURACV_OTA_HOST_BUILD) + OpenSSL libcrypto.

#include "../arduino/canary_wap/evidence_update_verify.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(cond) do { \
  if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
  checks++; \
} while (0)

// ── OpenSSL glue ────────────────────────────────────────────────────────────
static EVP_PKEY* g_key = nullptr;

static void make_key(uint8_t pub_out[32]) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  CHECK(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &g_key) == 1);
  EVP_PKEY_CTX_free(ctx);
  size_t len = 32;
  CHECK(EVP_PKEY_get_raw_public_key(g_key, pub_out, &len) == 1 && len == 32);
}

static void sign(const uint8_t* msg, size_t len, uint8_t sig_out[64]) {
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  size_t sig_len = 64;
  CHECK(EVP_DigestSignInit(md, nullptr, nullptr, nullptr, g_key) == 1);
  CHECK(EVP_DigestSign(md, sig_out, &sig_len, msg, len) == 1 && sig_len == 64);
  EVP_MD_CTX_free(md);
}

static bool dep_verify(const uint8_t sig[64], const uint8_t pub[32],
                       const uint8_t* msg, size_t msg_len) {
  EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
  if (!pk) return false;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  bool ok = EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pk) == 1 &&
            EVP_DigestVerify(md, sig, 64, msg, msg_len) == 1;
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(pk);
  return ok;
}

static void dep_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  SHA256(data, len, out);
}

static void to_hex(const uint8_t* b, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

// Build a fully signed manifest for `image`, mirroring ota_release.py.
static securacv_ota_manifest_t signed_manifest(const std::vector<uint8_t>& image,
                                               const char* product,
                                               const char* version) {
  securacv_ota_manifest_t m;
  memset(&m, 0, sizeof m);
  snprintf(m.product, sizeof m.product, "%s", product);
  snprintf(m.version, sizeof m.version, "%s", version);
  snprintf(m.url, sizeof m.url, "https://example.invalid/%s.bin", product);
  m.size = (uint32_t)image.size();

  uint8_t sha[32];
  dep_sha256(image.data(), image.size(), sha);
  to_hex(sha, 32, m.sha256);

  uint8_t signed_msg[36];
  securacv_ota_build_signed_message(m.size, sha, signed_msg);
  uint8_t sig[64];
  sign(signed_msg, sizeof signed_msg, sig);
  to_hex(sig, 64, m.signature);

  uint8_t canon[SECURACV_OTA_MANIFEST_MSG_MAX];
  size_t canon_len = 0;
  CHECK(securacv_ota_build_manifest_message(&m, canon, sizeof canon, &canon_len));
  uint8_t msig[64];
  sign(canon, canon_len, msig);
  to_hex(msig, 64, m.manifest_signature);
  return m;
}

int main() {
  uint8_t pubkey[32];
  make_key(pubkey);

  std::vector<uint8_t> image(150000);
  for (size_t i = 0; i < image.size(); i++) image[i] = (uint8_t)(i * 131 + 7);

  evidence_update::VerifyDeps deps;
  deps.ed25519_verify = dep_verify;
  deps.sha256 = dep_sha256;
  deps.release_pubkey = pubkey;
  deps.product = "securacv-canary-wap";
  deps.running_version = "2.2.0-wap";
  deps.nvs_floor = "";

  // ── the genuine article installs ──────────────────────────────────────────
  auto m = signed_manifest(image, "securacv-canary-wap", "2.3.0-wap");
  {
    auto v = evidence_update::verify(m, image.data(), (uint32_t)image.size(), deps);
    if (!v.ok) fprintf(stderr, "unexpected: %s %s\n", v.code, v.msg);
    CHECK(v.ok);
    CHECK(strcmp(v.code, "ok") == 0);
  }

  // ── every tampered variant is refused, with the right reason ──────────────
  { // unprovisioned key → fail closed
    evidence_update::VerifyDeps d2 = deps;
    uint8_t zero[32] = {0};
    d2.release_pubkey = zero;
    auto v = evidence_update::verify(m, image.data(), (uint32_t)image.size(), d2);
    CHECK(!v.ok && strcmp(v.code, "pubkey_missing") == 0);
  }
  { // wrong product
    auto m2 = signed_manifest(image, "securacv-canary-vision", "2.3.0");
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "product_mismatch") == 0);
  }
  { // flipped bit in the image → SHA mismatch
    auto bad = image;
    bad[12345] ^= 0x01;
    auto v = evidence_update::verify(m, bad.data(), (uint32_t)bad.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "sha_mismatch") == 0);
  }
  { // truncated image → size mismatch
    auto v = evidence_update::verify(m, image.data(), (uint32_t)image.size() - 1, deps);
    CHECK(!v.ok && strcmp(v.code, "size_mismatch") == 0);
  }
  { // edited manifest field after signing → manifest signature invalid
    auto m2 = m;
    snprintf(m2.release_notes, sizeof m2.release_notes, "totally legit");
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "manifest_sig_invalid") == 0);
  }
  { // version edit is also caught by the manifest signature (before version policy)
    auto m2 = m;
    snprintf(m2.version, sizeof m2.version, "9.9.9");
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "manifest_sig_invalid") == 0);
  }
  { // image signature stripped
    auto m2 = m;
    m2.signature[0] = '\0';
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "image_sig_format") == 0);
  }
  { // image signature swapped for a signature by a DIFFERENT key
    EVP_PKEY* old = g_key; g_key = nullptr;
    uint8_t other_pub[32];
    make_key(other_pub);
    auto m2 = signed_manifest(image, "securacv-canary-wap", "2.3.0-wap");
    EVP_PKEY_free(g_key); g_key = old;
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "manifest_sig_invalid") == 0);
  }
  { // same version already running → refused as up-to-date
    auto m2 = signed_manifest(image, "securacv-canary-wap", "2.2.0-wap");
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "up_to_date") == 0);
  }
  { // older than the running version → anti-rollback
    auto m2 = signed_manifest(image, "securacv-canary-wap", "2.1.0-wap");
    auto v = evidence_update::verify(m2, image.data(), (uint32_t)image.size(), deps);
    CHECK(!v.ok && strcmp(v.code, "rollback") == 0);
  }
  { // NVS floor above the offered version → anti-rollback even if newer than running
    evidence_update::VerifyDeps d2 = deps;
    d2.nvs_floor = "2.4.0";
    auto v = evidence_update::verify(m, image.data(), (uint32_t)image.size(), d2);
    CHECK(!v.ok && strcmp(v.code, "rollback") == 0);
  }

  printf("test_evidence_update_verify: %d checks passed\n", checks);
  return 0;
}
