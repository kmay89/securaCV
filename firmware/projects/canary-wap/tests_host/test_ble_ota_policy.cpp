// Host test for the BLE OTA header policy (ble_ota_policy.h): the 168-byte
// protocol-v2 header's layout and strict parse, the domain-separated
// canonical message pinned against the cross-language fixture in
// firmware/scripts/test_ota_release.py, and the accept / refuse /
// break-glass decision matrix with real Ed25519 signatures (OpenSSL) — the
// same injected-primitive shape the firmware fills with Crypto's
// Ed25519::verify. Links the OTA engine's pure subset for
// securacv_ota_update_decision(), so the floor rule under test is the pull
// path's own.
//
// Build (also in the tests_host Makefile and firmware.yml): g++ -std=c++17
// -Wall -Wextra -Werror -DSECURACV_OTA_HOST_BUILD, with the sketch dir on
// the include path, this file plus the sketch's securacv_ota.cpp, -lcrypto.

#include "ble_ota_policy.h"

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int tests_run = 0;
#define CHECK(cond) do { \
  tests_run++; \
  if (!(cond)) { fprintf(stderr, "FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
} while (0)

using namespace ble_ota;

// ── OpenSSL glue ────────────────────────────────────────────────────────────

struct Key {
  EVP_PKEY* pkey;
  uint8_t   pub[32];
};

static Key make_key() {
  Key k;
  k.pkey = nullptr;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  CHECK(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &k.pkey) == 1);
  EVP_PKEY_CTX_free(ctx);
  size_t len = 32;
  CHECK(EVP_PKEY_get_raw_public_key(k.pkey, k.pub, &len) == 1 && len == 32);
  return k;
}

static void sign(const Key& k, const uint8_t* msg, size_t len, uint8_t sig_out[64]) {
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  size_t sig_len = 64;
  CHECK(EVP_DigestSignInit(md, nullptr, nullptr, nullptr, k.pkey) == 1);
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

// ── fixtures ────────────────────────────────────────────────────────────────

static const char* PRODUCT = "securacv-canary-wap";
static const char* RUNNING = "2.4.15-wap";

static void fill_sha(uint8_t sha[32]) {
  for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(0xA0 + i);
}

// A well-formed, correctly signed v2 header.
static void make_v2(uint8_t out[168], const Key& k, const char* product,
                    const char* version, uint32_t size, const uint8_t sha[32]) {
  memset(out, 0, 168);
  out[0] = OTA_V2_MAGIC0;
  out[1] = OTA_V2_MAGIC1;
  out[2] = OTA_V2_HDR_VERSION;
  out[3] = 0;
  memcpy(out + 4, product, strlen(product));
  memcpy(out + 36, version, strlen(version));
  out[68] = (uint8_t)(size & 0xFF);
  out[69] = (uint8_t)((size >> 8) & 0xFF);
  out[70] = (uint8_t)((size >> 16) & 0xFF);
  out[71] = (uint8_t)((size >> 24) & 0xFF);
  memcpy(out + 72, sha, 32);

  uint8_t msg[OTA_V2_MSG_MAX];
  size_t msg_len = 0;
  CHECK(build_v2_message(product, version, size, sha, msg, sizeof(msg), &msg_len));
  sign(k, msg, msg_len, out + 104);
}

// A correctly signed legacy v1 header (signature over size || sha256 only).
static void make_v1(uint8_t out[132], const Key& k, uint32_t size,
                    const uint8_t sha[32], const char* version) {
  memset(out, 0, 132);
  memcpy(out + 0, &size, 4);  // host is little-endian in CI; also asserted below
  memcpy(out + 4, sha, 32);
  uint8_t msg[36];
  securacv_ota_build_signed_message(size, sha, msg);
  sign(k, msg, sizeof(msg), out + 36);
  memcpy(out + 100, version, strlen(version));
}

static PolicyDeps deps_for(const Key& k, const char* nvs_floor) {
  PolicyDeps d;
  d.ed25519_verify  = dep_verify;
  d.release_pubkey  = k.pub;
  d.product         = PRODUCT;
  d.running_version = RUNNING;
  d.nvs_floor       = nvs_floor;
  return d;
}

static std::string hex(const uint8_t* b, size_t n) {
  std::string s;
  char tmp[3];
  for (size_t i = 0; i < n; i++) { snprintf(tmp, sizeof(tmp), "%02x", b[i]); s += tmp; }
  return s;
}

// ── 1. wire layout ──────────────────────────────────────────────────────────

static void test_layout() {
  CHECK(sizeof(OtaHeader) == 132);
  CHECK(sizeof(OtaHeaderV2) == 168);
  CHECK(offsetof(OtaHeaderV2, magic) == 0);
  CHECK(offsetof(OtaHeaderV2, hdr_version) == 2);
  CHECK(offsetof(OtaHeaderV2, reserved) == 3);
  CHECK(offsetof(OtaHeaderV2, product) == 4);
  CHECK(offsetof(OtaHeaderV2, version) == 36);
  CHECK(offsetof(OtaHeaderV2, image_size) == 68);
  CHECK(offsetof(OtaHeaderV2, sha256) == 72);
  CHECK(offsetof(OtaHeaderV2, signature) == 104);
  CHECK(CMD_BEGIN_V1 == 0x01 && CMD_ABORT == 0x02 && CMD_BEGIN_V2 == 0x03);
  // Byte-order sanity for make_v1's raw memcpy of the size.
  uint32_t probe = 1;
  CHECK(*(const uint8_t*)&probe == 1);
}

// ── 2. canonical message — cross-language fixture ───────────────────────────

static void test_canonical_fixture() {
  // Shared with firmware/scripts/test_ota_release.py
  // (TestBleOtaHeader::test_canonical_message_cross_language_fixture). If
  // either side drifts, every device refuses every signed BLE header.
  uint8_t sha[32];
  const std::string sha_hex = "AAbb" + std::string(60, '0');  // mixed case → lowercased
  CHECK(securacv_ota_hex_to_bytes(sha_hex.c_str(), sha, 32));

  uint8_t msg[OTA_V2_MSG_MAX];
  size_t msg_len = 0;
  CHECK(build_v2_message("securacv-canary-wap", "2.4.15-wap", 123456, sha,
                         msg, sizeof(msg), &msg_len));
  CHECK(msg_len == 118);
  CHECK(hex(msg, msg_len) ==
        "7363762d626c652d6f74612d76320073656375726163762d63616e6172792d77617000"
        "322e342e31352d7761700031323334353600"
        "61616262303030303030303030303030303030303030303030303030303030303030"
        "30303030303030303030303030303030303030303030303030303030303000");
  // Structure: domain prefix, then NUL-separated fields, sha hex lowercased.
  CHECK(memcmp(msg, "scv-ble-ota-v2", 15) == 0);  // includes the NUL
  CHECK(msg[msg_len - 1] == '\0');

  // Capacity is honored: one byte short fails, exact fits.
  uint8_t small[117];
  CHECK(!build_v2_message("securacv-canary-wap", "2.4.15-wap", 123456, sha,
                          small, sizeof(small), &msg_len));
  uint8_t exact[118];
  CHECK(build_v2_message("securacv-canary-wap", "2.4.15-wap", 123456, sha,
                         exact, sizeof(exact), &msg_len) && msg_len == 118);
  // Worst case fits the documented bound.
  const std::string p31(31, 'p'), v31(31, '9');
  CHECK(build_v2_message(p31.c_str(), v31.c_str(), 0xFFFFFFFFu, sha,
                         msg, sizeof(msg), &msg_len) && msg_len == 155);
  CHECK(!build_v2_message(nullptr, "1.0", 1, sha, msg, sizeof(msg), &msg_len));
}

// ── 3. v2 parsing is strict ─────────────────────────────────────────────────

static void test_parse_v2() {
  Key k = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[168];
  make_v2(h, k, PRODUCT, "2.5.0-wap", 0x00123456u, sha);

  ParsedHeader p;
  CHECK(parse_v2(h, sizeof(h), &p));
  CHECK(p.kind == HDR_V2);
  CHECK(strcmp(p.product, PRODUCT) == 0);
  CHECK(strcmp(p.version, "2.5.0-wap") == 0);
  CHECK(p.image_size == 0x00123456u);
  CHECK(memcmp(p.sha256, sha, 32) == 0);
  CHECK(memcmp(p.signature, h + 104, 64) == 0);

  // Length must be exact.
  CHECK(!parse_v2(h, 167, &p));
  CHECK(!parse_v2(h, 169, &p));
  CHECK(!parse_v2(nullptr, 168, &p));

  // Each preamble byte is checked.
  uint8_t m[168];
  memcpy(m, h, 168); m[0] = 0x52; CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); m[1] = 0x44; CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); m[2] = 0x01; CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); m[2] = 0x03; CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); m[3] = 0x01; CHECK(!parse_v2(m, 168, &p));

  // Product / version field hygiene.
  memcpy(m, h, 168); memset(m + 4, 'x', 32);        CHECK(!parse_v2(m, 168, &p));  // no NUL in slot
  memcpy(m, h, 168); m[4] = '\0';                   CHECK(!parse_v2(m, 168, &p));  // empty product
  memcpy(m, h, 168); m[12] = ' ';                   CHECK(!parse_v2(m, 168, &p));  // space
  memcpy(m, h, 168); m[12] = (uint8_t)0x80;         CHECK(!parse_v2(m, 168, &p));  // non-ASCII
  memcpy(m, h, 168); m[12] = 0x09;                  CHECK(!parse_v2(m, 168, &p));  // control
  memcpy(m, h, 168); m[4 + 31] = 'z';               CHECK(!parse_v2(m, 168, &p));  // junk in padding
  memcpy(m, h, 168); m[36] = '\0';                  CHECK(!parse_v2(m, 168, &p));  // empty version
  memcpy(m, h, 168); m[36 + 20] = 'q';              CHECK(!parse_v2(m, 168, &p));  // junk in padding
  memcpy(m, h, 168); memcpy(m + 36, "wap\0\0\0\0\0\0", 9);   CHECK(!parse_v2(m, 168, &p));  // no numeric prefix
  memcpy(m, h, 168); memcpy(m + 36, "2\0\0\0\0\0\0\0\0", 9); CHECK(!parse_v2(m, 168, &p));  // no minor
  memcpy(m, h, 168); memcpy(m + 36, "2.\0\0\0\0\0\0\0", 9);  CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); memcpy(m + 36, "v2.5.0\0\0\0", 9);      CHECK(!parse_v2(m, 168, &p));
  memcpy(m, h, 168); memcpy(m + 36, "2.5\0\0\0\0\0\0", 9);   CHECK(parse_v2(m, 168, &p));   // major.minor is enough

  // Exactly 31 chars + NUL is the widest legal field.
  const std::string p31(31, 'p');
  make_v2(h, k, p31.c_str(), "1.2.3", 1, sha);
  CHECK(parse_v2(h, 168, &p) && strcmp(p.product, p31.c_str()) == 0);

  EVP_PKEY_free(k.pkey);
}

// ── 4. v1 parsing (legacy, display-only version) ────────────────────────────

static void test_parse_v1() {
  Key k = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[132];
  make_v1(h, k, 4096, sha, "2.4.14-wap");

  ParsedHeader p;
  CHECK(!parse_v1(h, 131, &p));
  CHECK(parse_v1(h, 132, &p));
  CHECK(p.kind == HDR_V1);
  CHECK(p.product[0] == '\0');
  CHECK(p.image_size == 4096);
  CHECK(memcmp(p.sha256, sha, 32) == 0);
  CHECK(strcmp(p.version, "2.4.14-wap") == 0);
  CHECK(parse_v1(h, 140, &p));  // trailing bytes tolerated, as before

  // Unsigned version bytes are sanitized, never trusted.
  memset(h + 100, 0, 32);
  memcpy(h + 100, "bad\x01\x7fver\n", 9);
  CHECK(parse_v1(h, 132, &p) && strcmp(p.version, "bad__ver_") == 0);
  memset(h + 100, 'V', 32);  // no terminator in the slot
  CHECK(parse_v1(h, 132, &p) && strlen(p.version) == 31);

  EVP_PKEY_free(k.pkey);
}

// ── 5. v2 decision: floor ───────────────────────────────────────────────────

static void test_decide_v2_floor() {
  Key k = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[168];
  ParsedHeader p;

  // Above the running version, no NVS floor → accept.
  make_v2(h, k, PRODUCT, "2.5.0-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  Verdict v = decide(p, deps_for(k, ""));
  CHECK(v.decision == DECISION_ACCEPT && v.need == BG_NONE && strcmp(v.reason, REASON_OK) == 0);
  CHECK(apply_break_glass(v, false).decision == DECISION_ACCEPT);  // pass-through

  // Equal to the running version → accept (a re-flash is a repair).
  make_v2(h, k, PRODUCT, RUNNING, 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, nullptr)).decision == DECISION_ACCEPT);

  // Below the running version → needs break-glass; refused unless armed.
  make_v2(h, k, PRODUCT, "2.4.14-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  v = decide(p, deps_for(k, ""));
  CHECK(v.decision == DECISION_NEEDS_BREAK_GLASS && v.need == BG_FLOOR);
  CHECK(strcmp(v.reason, REASON_BREAK_GLASS_FLOOR) == 0);
  Verdict r = apply_break_glass(v, false);
  CHECK(r.decision == DECISION_REFUSE && strcmp(r.reason, REASON_FLOOR_REFUSED) == 0);
  Verdict a = apply_break_glass(v, true);
  CHECK(a.decision == DECISION_ACCEPT_BREAK_GLASS && a.need == BG_FLOOR);
  CHECK(strcmp(a.reason, REASON_BREAK_GLASS_FLOOR) == 0);

  // The NVS floor wins when it is above the running version (same rule as
  // the pull path: max(running, floor)).
  make_v2(h, k, PRODUCT, "2.5.0-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "2.6.0-wap")).decision == DECISION_NEEDS_BREAK_GLASS);
  make_v2(h, k, PRODUCT, "2.6.0-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "2.6.0-wap")).decision == DECISION_ACCEPT);   // at the floor
  make_v2(h, k, PRODUCT, "2.6.1-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "2.6.0-wap")).decision == DECISION_ACCEPT);

  // Prerelease ranking is the engine's: a dev build below the stable it
  // shares a triple with is a rollback.
  make_v2(h, k, PRODUCT, "2.4.15-dev.3", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "")).decision == DECISION_NEEDS_BREAK_GLASS);

  EVP_PKEY_free(k.pkey);
}

// ── 6. v2 decision: product binding is never bypassed ───────────────────────

static void test_decide_v2_product() {
  Key k = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[168];
  ParsedHeader p;

  make_v2(h, k, "securacv-canary", "9.9.9", 1000, sha);   // newer, but another product
  CHECK(parse_v2(h, 168, &p));
  Verdict v = decide(p, deps_for(k, ""));
  CHECK(v.decision == DECISION_REFUSE && strcmp(v.reason, REASON_PRODUCT) == 0);
  CHECK(apply_break_glass(v, true).decision == DECISION_REFUSE);  // arming changes nothing

  // Prefix / suffix variants are not the product.
  make_v2(h, k, "securacv-canary-wap2", "9.9.9", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "")).decision == DECISION_REFUSE);
  make_v2(h, k, "securacv-canary-wa", "9.9.9", 1000, sha);
  CHECK(parse_v2(h, 168, &p));
  CHECK(decide(p, deps_for(k, "")).decision == DECISION_REFUSE);

  EVP_PKEY_free(k.pkey);
}

// ── 7. v2 decision: every signed field is bound ─────────────────────────────

static void test_decide_v2_signature() {
  Key k = make_key();
  Key other = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[168];
  uint8_t m[168];
  ParsedHeader p;
  PolicyDeps d = deps_for(k, "");

  make_v2(h, k, PRODUCT, "2.5.0-wap", 1000, sha);

  // Version edited after signing (still a clean field) → signature fails.
  memcpy(m, h, 168); m[36 + 4] = '1';               // 2.5.1-wap
  CHECK(parse_v2(m, 168, &p));
  Verdict v = decide(p, d);
  CHECK(v.decision == DECISION_REFUSE && strcmp(v.reason, REASON_SIGNATURE) == 0);
  CHECK(apply_break_glass(v, true).decision == DECISION_REFUSE);

  // Product edited (same length, still clean) → signature fails, and it
  // fails BEFORE the product check.
  memcpy(m, h, 168); m[4 + 18] = 'x';               // securacv-canary-wax
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // Size edited → signature fails.
  memcpy(m, h, 168); m[68] ^= 0x01;
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // Digest edited → signature fails.
  memcpy(m, h, 168); m[72 + 5] ^= 0x80;
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // Signature bit flipped → fails.
  memcpy(m, h, 168); m[104 + 63] ^= 0x01;
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // Signed by a key that is not the release key → fails.
  make_v2(m, other, PRODUCT, "2.5.0-wap", 1000, sha);
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // A v1-style signature (over size||sha only) pasted into a v2 header
  // does not verify against the domain-separated message.
  make_v2(m, k, PRODUCT, "2.5.0-wap", 1000, sha);
  {
    uint8_t v1msg[36];
    securacv_ota_build_signed_message(1000, sha, v1msg);
    sign(k, v1msg, sizeof(v1msg), m + 104);
  }
  CHECK(parse_v2(m, 168, &p));
  CHECK(strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  // And the untouched header still passes (the tampering above was the
  // only reason for the refusals).
  CHECK(parse_v2(h, 168, &p) && decide(p, d).decision == DECISION_ACCEPT);

  EVP_PKEY_free(k.pkey);
  EVP_PKEY_free(other.pkey);
}

// ── 8. v1 decision: break-glass only, signature still required ──────────────

static void test_decide_v1() {
  Key k = make_key();
  Key other = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[132];
  ParsedHeader p;
  PolicyDeps d = deps_for(k, "");

  // Valid release signature, even for a NEWER version string: v1 carries
  // no product and an unsigned version, so it always needs break-glass.
  make_v1(h, k, 2048, sha, "9.9.9-wap");
  CHECK(parse_v1(h, 132, &p));
  Verdict v = decide(p, d);
  CHECK(v.decision == DECISION_NEEDS_BREAK_GLASS && v.need == BG_V1);
  CHECK(strcmp(v.reason, REASON_BREAK_GLASS_V1) == 0);
  Verdict r = apply_break_glass(v, false);
  CHECK(r.decision == DECISION_REFUSE && strcmp(r.reason, REASON_V1_REFUSED) == 0);
  Verdict a = apply_break_glass(v, true);
  CHECK(a.decision == DECISION_ACCEPT_BREAK_GLASS && a.need == BG_V1);

  // Wrong key → refused, armed or not.
  make_v1(h, other, 2048, sha, "9.9.9-wap");
  CHECK(parse_v1(h, 132, &p));
  v = decide(p, d);
  CHECK(v.decision == DECISION_REFUSE && strcmp(v.reason, REASON_SIGNATURE) == 0);
  CHECK(apply_break_glass(v, true).decision == DECISION_REFUSE);

  // Size or digest edited after signing → refused.
  make_v1(h, k, 2048, sha, "1.0.0");
  h[0] ^= 0x01;
  CHECK(parse_v1(h, 132, &p) && strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);
  make_v1(h, k, 2048, sha, "1.0.0");
  h[4] ^= 0x01;
  CHECK(parse_v1(h, 132, &p) && strcmp(decide(p, d).reason, REASON_SIGNATURE) == 0);

  EVP_PKEY_free(k.pkey);
  EVP_PKEY_free(other.pkey);
}

// ── 9. gates that precede every header check ────────────────────────────────

static void test_decide_gates() {
  Key k = make_key();
  uint8_t sha[32]; fill_sha(sha);
  uint8_t h[168];
  ParsedHeader p;
  make_v2(h, k, PRODUCT, "2.5.0-wap", 1000, sha);
  CHECK(parse_v2(h, 168, &p));

  // All-zero release key → refused (fail-closed), armed or not.
  static const uint8_t zero[32] = {0};
  PolicyDeps d = deps_for(k, "");
  d.release_pubkey = zero;
  Verdict v = decide(p, d);
  CHECK(v.decision == DECISION_REFUSE && strcmp(v.reason, REASON_UNPROVISIONED) == 0);
  CHECK(apply_break_glass(v, true).decision == DECISION_REFUSE);
  d.release_pubkey = nullptr;
  CHECK(strcmp(decide(p, d).reason, REASON_UNPROVISIONED) == 0);
  d = deps_for(k, ""); d.ed25519_verify = nullptr;
  CHECK(strcmp(decide(p, d).reason, REASON_UNPROVISIONED) == 0);

  // Policy not bound (sketch never called configure()) → refused.
  d = deps_for(k, ""); d.product = nullptr;
  CHECK(strcmp(decide(p, d).reason, REASON_UNCONFIGURED) == 0);
  d = deps_for(k, ""); d.product = "";
  CHECK(strcmp(decide(p, d).reason, REASON_UNCONFIGURED) == 0);
  d = deps_for(k, ""); d.running_version = nullptr;
  CHECK(strcmp(decide(p, d).reason, REASON_UNCONFIGURED) == 0);

  // A header that never parsed is malformed, whatever else is set.
  ParsedHeader none;
  memset(&none, 0, sizeof(none));
  none.kind = HDR_NONE;
  v = decide(none, deps_for(k, ""));
  CHECK(v.decision == DECISION_REFUSE && strcmp(v.reason, REASON_MALFORMED) == 0);

  EVP_PKEY_free(k.pkey);
}

// ── 10. reason strings: distinct and small enough for last_error[64] ────────

static void test_reason_strings() {
  const char* all[] = {
    REASON_OK, REASON_UNPROVISIONED, REASON_UNCONFIGURED, REASON_MALFORMED,
    REASON_SIGNATURE, REASON_PRODUCT, REASON_V1_REFUSED, REASON_FLOOR_REFUSED,
    REASON_BREAK_GLASS_V1, REASON_BREAK_GLASS_FLOOR,
  };
  const size_t n = sizeof(all) / sizeof(all[0]);
  for (size_t i = 0; i < n; i++) {
    CHECK(strlen(all[i]) < 64);
    for (size_t j = i + 1; j < n; j++) CHECK(strcmp(all[i], all[j]) != 0);
  }
}

int main() {
  test_layout();
  test_canonical_fixture();
  test_parse_v2();
  test_parse_v1();
  test_decide_v2_floor();
  test_decide_v2_product();
  test_decide_v2_signature();
  test_decide_v1();
  test_decide_gates();
  test_reason_strings();
  printf("ALL %d BLE OTA POLICY CHECKS PASSED\n", tests_run);
  return 0;
}
