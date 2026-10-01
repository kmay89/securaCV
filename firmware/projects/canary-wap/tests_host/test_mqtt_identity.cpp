/**
 * @file test_mqtt_identity.cpp
 * @brief Sweep HA20: the canary-wap publishes its MQTT fingerprint and
 *        health key in lowercase hex, 16 and 64 digits, like every other
 *        build.
 *
 * Through firmware 2.4.15 canary_wap.ino spelled the envelope `fp` of every
 * signed chain / counts / events publish, and the health `public_key`,
 * with hex_to_str, which writes capitals. Home Assistant and the
 * canary-display learned to ignore the case (HA18, HA19) and keep doing so
 * for deployed units. The sketch now spells those two strings with
 * mqtt_identity.h and nothing else.
 *
 * Three parts:
 *
 *  1. The encoder, on the repo's Ed25519 test key (seed 0x42 * 32, the key
 *     custom_components/securacv/tests/test_fingerprint_case.py uses). Its
 *     fingerprint is derived here with OpenSSL the way compute_fingerprint
 *     derives it (SHA-256 over "securacv:pubkey:fingerprint", a NUL, the
 *     key; first 8 bytes), so the expected strings are not typed from the
 *     encoder's own output.
 *  2. The envelope: device_signature::init takes the encoder's fp (as the
 *     sketch now hands it), and an events body built through the real
 *     csi_event_wire.h with device_signature::fingerprint_hex(), signed with
 *     OpenSSL's deterministic Ed25519 over device_signature's own event
 *     canonical, is byte for byte the lowercase body test_fingerprint_case.py
 *     runs through Home Assistant.
 *  3. Source pins (beacon_source_scan.h): canary_wap.ino feeds both
 *     csi_mqtt::init calls and its one device_signature::init call from the
 *     encoder, and csi_mqtt.cpp takes every published fp from
 *     device_signature::fingerprint_hex() and the health key only from what
 *     csi_mqtt::init was handed. The Makefile passes both paths, and every
 *     pin fails closed when a file cannot be read.
 *
 * Host-tested only: the sketch itself compiles in CI's Arduino leg, and
 * nothing here has run on a unit.
 *
 * Run via: `make -C firmware/projects/canary-wap/tests_host`.
 */

#include "mqtt_identity.h"
#include "device_signature.h"
#include "csi_event_wire.h"
#include "beacon_source_scan.h"

#include <openssl/evp.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef CANARY_WAP_INO
#error "the Makefile passes CANARY_WAP_INO, the sketch's absolute path"
#endif
#ifndef CSI_MQTT_CPP
#error "the Makefile passes CSI_MQTT_CPP, csi_mqtt.cpp's absolute path"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* what) {
  ++g_checks;
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
  ++g_checks;
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  got:  '%s'\n  want: '%s'\n",
                 what, got.c_str(), want.c_str());
    ++g_failures;
  }
}

// The repo's test key and what Home Assistant's fixture says it spells.
const char kTestPub[] =
    "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12";
const char kTestFp[] = "7916ca487912fa1b";
// generate_device_id() for this key on an S3 board (test_fingerprint_case.py).
const char kDeviceId[] = "canary-s3-4dC2";

// test_fingerprint_case.py's WAP_EVENT, as a canary-wap after HA20 sends it:
// the fp in lowercase, every other byte as before (the signature does not
// cover the fp).
const char kLowercaseEventBody[] =
    "{\"event_id\":77,\"event_type\":\"present\",\"timestamp\":98,\"zone\":\"\","
    "\"confidence\":\"likely\",\"signed\":true,\"module\":\"core.presence\","
    "\"type\":\"presence\",\"category\":\"event\",\"privacy\":\"p1\","
    "\"state\":\"present\",\"motion\":42,\"breathing\":17,\"bpm\":14,"
    "\"duration_sec\":120,\"bundled\":1,\"replay\":false,\"v\":1,"
    "\"alg\":\"ed25519\",\"fp\":\"7916ca487912fa1b\","
    "\"sig\":\"MKYFGqnCMgTbqZH83Z9bvH-_WktERv_RR7ubjarog7i28YlWpsDo_Z9zjLuovY9_"
    "h7foHkUY1pjIYaBA89m6Bg\"}";

bool lowercase_hex(const char* s, size_t want_len) {
  if (std::strlen(s) != want_len) return false;
  for (size_t i = 0; i < want_len; i++) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// ── the test key, with OpenSSL ─────────────────────────────────────────────

struct TestKey {
  uint8_t seed[32];
  uint8_t pub[32];
  uint8_t fp[8];
  EVP_PKEY* pkey = nullptr;
  ~TestKey() { if (pkey) EVP_PKEY_free(pkey); }
};

bool make_test_key(TestKey& k) {
  std::memset(k.seed, 0x42, sizeof(k.seed));
  k.pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, k.seed, 32);
  size_t len = 32;
  if (!k.pkey || EVP_PKEY_get_raw_public_key(k.pkey, k.pub, &len) != 1 || len != 32) {
    return false;
  }
  // compute_fingerprint (canary_wap.ino): sha256_domain(domain, pub) is
  // SHA-256(domain || 0x00 || pub); the fingerprint is its first 8 bytes.
  static const char kDomain[] = "securacv:pubkey:fingerprint";
  const uint8_t nul = 0;
  uint8_t hash[32];
  unsigned int hlen = 0;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok = md &&
      EVP_DigestInit_ex(md, EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(md, kDomain, std::strlen(kDomain)) == 1 &&
      EVP_DigestUpdate(md, &nul, 1) == 1 &&
      EVP_DigestUpdate(md, k.pub, 32) == 1 &&
      EVP_DigestFinal_ex(md, hash, &hlen) == 1 && hlen == 32;
  EVP_MD_CTX_free(md);
  if (!ok) return false;
  std::memcpy(k.fp, hash, 8);
  return true;
}

// ── 1. the encoder ─────────────────────────────────────────────────────────

void test_encoder_spells_the_test_key_in_lowercase(const TestKey& k) {
  char key_hex[mqtt_identity::KEY_HEX_CAP];
  mqtt_identity::public_key_hex(key_hex, k.pub);
  check(lowercase_hex(key_hex, 64), "health public_key is 64 lowercase hex digits");
  check_eq(key_hex, kTestPub, "health public_key is the test key, lowercase");

  char fp_hex[mqtt_identity::FP_HEX_CAP];
  mqtt_identity::fingerprint_hex(fp_hex, k.fp);
  check(lowercase_hex(fp_hex, 16), "envelope fp is 16 lowercase hex digits");
  check_eq(fp_hex, kTestFp, "envelope fp is HA's fingerprint_from_pubkey_hex of the key");
}

void test_encoder_every_byte_value() {
  // Every nibble maps to a lowercase digit and decodes back to itself.
  uint8_t all[256];
  for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
  char out[2 * 256 + 1];
  std::memset(out, 'X', sizeof(out));
  mqtt_identity::hex_lower(out, all, 256);
  check(lowercase_hex(out, 512), "every byte value spells as two lowercase hex digits");
  bool round_trip = true;
  for (int i = 0; i < 256; i++) {
    unsigned v = 0;
    if (std::sscanf(out + 2 * i, "%2x", &v) != 1 || v != (unsigned)i) round_trip = false;
  }
  check(round_trip, "every byte value decodes back to itself");
  check(out[512] == '\0', "the encoder terminates the string");

  // The buffers the sketch declares hold exactly the digits and the NUL.
  check(mqtt_identity::FP_HEX_CAP == 17, "FP_HEX_CAP is 16 digits + NUL");
  check(mqtt_identity::KEY_HEX_CAP == 65, "KEY_HEX_CAP is 64 digits + NUL");
}

// ── 2. the envelope ────────────────────────────────────────────────────────

const TestKey* g_key = nullptr;

// device_signature::sign_event's contract, with OpenSSL's Ed25519 in place of
// the rweather library the firmware links (RFC 8032 Ed25519 is
// deterministic, so both write the same signature). The canonical is
// device_signature's own builder, fed the device_id init cached.
bool openssl_sign_event(uint32_t event_id, const char* state, const char* category,
                        const char* privacy, int motion, int breath, int bpm,
                        char* sig_out, size_t sig_cap) {
  char canonical[256];
  const size_t n = device_signature::build_event_canonical(
      event_id, state, category, privacy, motion, breath, bpm,
      device_signature::device_id(), canonical, sizeof(canonical));
  if (n == 0 || !g_key) return false;
  uint8_t sig[64];
  size_t sig_len = sizeof(sig);
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok = md &&
      EVP_DigestSignInit(md, nullptr, nullptr, nullptr, g_key->pkey) == 1 &&
      EVP_DigestSign(md, sig, &sig_len,
                     reinterpret_cast<const uint8_t*>(canonical), n) == 1 &&
      sig_len == 64;
  EVP_MD_CTX_free(md);
  if (!ok) return false;
  return device_signature::b64url_encode_nopad(sig, 64, sig_out, sig_cap) ==
         device_signature::SIG_B64URL_LEN;
}

void test_signed_events_body_carries_the_lowercase_fp(const TestKey& k) {
  // As canary_wap.ino's register_api_routes now hands it over.
  char fp_hex[mqtt_identity::FP_HEX_CAP];
  mqtt_identity::fingerprint_hex(fp_hex, k.fp);
  device_signature::init(k.seed, k.pub, kDeviceId, fp_hex);

  check_eq(device_signature::fingerprint_hex(), kTestFp,
           "device_signature caches the lowercase fp (the envelope fp)");
  check(lowercase_hex(device_signature::fingerprint_hex(), 16),
        "the cached fp is 16 lowercase hex digits");
  // /enroll and /api/device/enroll print the same pair: the key was already
  // lowercase there (device_signature's own encoder), and now the fp is too.
  char key_hex[mqtt_identity::KEY_HEX_CAP];
  mqtt_identity::public_key_hex(key_hex, k.pub);
  check_eq(device_signature::pubkey_hex(), key_hex,
           "the health key and the enroll card's key are one spelling");

  // csi_mqtt.cpp's Signer, field for field.
  g_key = &k;
  const csi_event_wire::Signer signer = {
    &openssl_sign_event,
    device_signature::SCHEMA_V,
    device_signature::ALG_NAME,
    device_signature::fingerprint_hex(),
  };
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));
  v.category = CSI_CATEGORY_EVENT;
  std::strcpy(v.state_name, "present");
  std::strcpy(v.confidence, "likely");
  v.motion_score = 42;
  v.breathing_score = 17;
  v.breathing_rate_bpm = 14;
  v.duration_sec = 120;
  char body[768];
  const size_t n = csi_event_wire::build_event_body(
      body, sizeof(body), /*event_id=*/77, "core.presence", "presence",
      CSI_CATEGORY_EVENT, CSI_PRIVACY_P1, &v, /*timestamp_ms=*/98765,
      /*bundled_count=*/1, /*is_replay=*/false, signer);
  g_key = nullptr;
  check(n == std::strlen(body) && n > 0, "the events body was built");
  check_eq(body, kLowercaseEventBody,
           "the signed events body is HA's lowercase WAP_EVENT, byte for byte");
}

// ── 3. the call sites ──────────────────────────────────────────────────────

void test_sketch_feeds_both_strings_from_the_encoder() {
  bool ok = false;
  const std::string raw = beacon_source_scan::read_source(CANARY_WAP_INO, &ok);
  check(ok && !raw.empty(), "canary_wap.ino is readable");
  if (!ok) return;
  const std::string code =
      beacon_source_scan::squeeze(beacon_source_scan::strip_comments(raw));

  // Both csi_mqtt::init calls (at boot, and after a QR hub provision) take
  // the health key the encoder spelled on the line before.
  const std::string fed_init =
      "mqtt_identity::public_key_hex(pubkey_hex,g_device.pubkey);"
      "csi_mqtt::init(g_device.device_id,FIRMWARE_VERSION,pubkey_hex);";
  check(beacon_source_scan::count(code, "csi_mqtt::init(") == 2,
        "canary_wap.ino calls csi_mqtt::init twice (boot, QR hub provision)");
  check(beacon_source_scan::count(code, fed_init) == 2,
        "every csi_mqtt::init call takes the key mqtt_identity::public_key_hex spelled");

  // The one device_signature::init call takes the fp the encoder spelled:
  // the fp every signed publish carries.
  check(beacon_source_scan::count(code, "device_signature::init(") == 1,
        "canary_wap.ino calls device_signature::init once");
  check(code.find(
            "mqtt_identity::fingerprint_hex(mqtt_fp_hex,g_device.pubkey_fp);"
            "device_signature::init(g_device.privkey,g_device.pubkey,"
            "g_device.device_id,mqtt_fp_hex);") != std::string::npos,
        "device_signature::init takes the fp mqtt_identity::fingerprint_hex spelled");
}

void test_csi_mqtt_publishes_only_those_strings() {
  bool ok = false;
  const std::string raw = beacon_source_scan::read_source(CSI_MQTT_CPP, &ok);
  check(ok && !raw.empty(), "csi_mqtt.cpp is readable");
  if (!ok) return;
  const std::string code =
      beacon_source_scan::squeeze(beacon_source_scan::strip_comments(raw));

  // The chain and counts bodies write "fp" themselves; the events body gets
  // it from csi_event_wire's Signer. All three take device_signature's copy.
  check(beacon_source_scan::count(code, "\\\"fp\\\":\\\"%s\\\"") == 2,
        "csi_mqtt.cpp writes an fp into two bodies (chain, counts)");
  check(beacon_source_scan::count(code, "device_signature::fingerprint_hex()") == 3,
        "chain, counts and the events Signer take device_signature::fingerprint_hex()");

  // Two health formats (with and without a battery) print the key, and the
  // stored key is only ever copied from csi_mqtt::init's argument: erase the
  // reads and that one copy, and nothing may be left.
  check(beacon_source_scan::count(code, "\\\"public_key\\\":\\\"%s\\\"") == 2,
        "csi_mqtt.cpp writes the health key into two formats");
  check(beacon_source_scan::count(
            code, "strncpy(s_public_key_hex,public_key_hex,sizeof(s_public_key_hex)-1);") == 1,
        "csi_mqtt::init copies the key it was handed");
  std::string rest = code;
  for (const char* known : {
           "chars_public_key_hex[65]={};",
           "strncpy(s_public_key_hex,public_key_hex,sizeof(s_public_key_hex)-1);",
           "s_public_key_hex[sizeof(s_public_key_hex)-1]='\\0';",
           // both health snprintf calls and both re-inits end this way
           "s_firmware_version,s_public_key_hex);",
       }) {
    const std::string k = known;
    for (size_t at = rest.find(k); at != std::string::npos; at = rest.find(k, at)) {
      rest.erase(at, k.size());
    }
  }
  check(rest.find("s_public_key_hex") == std::string::npos,
        "nothing but csi_mqtt::init's copy writes the published health key");
}

}  // namespace

int main() {
  TestKey k;
  if (!make_test_key(k)) {
    std::fprintf(stderr, "FAIL: OpenSSL could not make the Ed25519 test key\n");
    return 1;
  }
  // Spelled by printf, not by the encoder under test.
  std::string pub_hex;
  for (uint8_t b : k.pub) {
    char two[3];
    std::snprintf(two, sizeof(two), "%02x", b);
    pub_hex += two;
  }
  check_eq(pub_hex, kTestPub, "OpenSSL's key for seed 0x42 * 32 is HA's TEST_PUB");

  test_encoder_spells_the_test_key_in_lowercase(k);
  test_encoder_every_byte_value();
  test_signed_events_body_carries_the_lowercase_fp(k);
  test_sketch_feeds_both_strings_from_the_encoder();
  test_csi_mqtt_publishes_only_those_strings();

  if (g_failures) {
    std::fprintf(stderr, "test_mqtt_identity: %d of %d checks FAILED\n",
                 g_failures, g_checks);
    return 1;
  }
  std::printf("ALL mqtt_identity TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
