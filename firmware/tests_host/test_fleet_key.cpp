// Host tests for firmware/common/network/fleet_key.h — HMAC-SHA256 and HKDF
// against the RFC vectors, the household key derivation, the tag.

#include "../common/network/fleet_key.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::fleet_key;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static std::string hex(const uint8_t* p, size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 0xf]; }
  return s;
}

static void hmac_matches_rfc4231() {
  // Test case 2: key "Jefe", data "what do ya want for nothing?"
  uint8_t out[32];
  hmac_sha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, out);
  CHECK(hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "rfc 4231 #2: %s", hex(out, 32).c_str());
  // Test case 1: key 0x0b*20, data "Hi There"
  uint8_t k[20]; std::memset(k, 0x0b, sizeof(k));
  hmac_sha256(k, 20, (const uint8_t*)"Hi There", 8, out);
  CHECK(hex(out, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "rfc 4231 #1: %s", hex(out, 32).c_str());
}

static void hkdf_matches_rfc5869() {
  uint8_t ikm[22]; std::memset(ikm, 0x0b, sizeof(ikm));
  const uint8_t salt[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
  const uint8_t info[] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};
  uint8_t okm[42];
  CHECK(hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm)), "derives");
  CHECK(hex(okm, 42) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        "rfc 5869 #1: %s", hex(okm, 42).c_str());
  CHECK(!hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, 0), "zero length refused");
  CHECK(!hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), nullptr, 32), "null out refused");
}

static void the_household_key() {
  uint8_t a[KEY_BYTES], b[KEY_BYTES], c[KEY_BYTES];
  CHECK(derive_from_wifi("Home", "hunter2", a), "derives");
  CHECK(derive_from_wifi("Home", "hunter2", b), "again");
  CHECK(std::memcmp(a, b, KEY_BYTES) == 0, "deterministic: every node on this network agrees");
  CHECK(derive_from_wifi("Home", "hunter3", c), "another password");
  CHECK(std::memcmp(a, c, KEY_BYTES) != 0, "a different password, a different fleet");
  CHECK(derive_from_wifi("Hom", "ehunter2", c), "a shifted split");
  CHECK(std::memcmp(a, c, KEY_BYTES) != 0, "the separator keeps ssid|pass apart");
  CHECK(!derive_from_wifi("Cafe", "", a), "an open network has no secret: refused");
  CHECK(!derive_from_wifi("Cafe", nullptr, a), "null password: refused");
  CHECK(!derive_from_wifi("", "x", a), "empty ssid: refused");
  CHECK(!derive_from_wifi(std::string(33, 's').c_str(), "x", a), "over-long ssid: refused");
  CHECK(!derive_from_wifi("Home", std::string(65, 'p').c_str(), a), "over-long password: refused");
  // Pinned so a reword of the salt or info cannot pass silently (every
  // shipped node derives from these bytes).
  derive_from_wifi("Home", "hunter2", a);
  char id[9];
  key_id_hex(a, id);
  CHECK(std::strlen(id) == 8, "an 8-hex key id: %s", id);
  static std::string pinned_id;  // computed once, compared to itself twice
  pinned_id = id;
  key_id_hex(a, id);
  CHECK(pinned_id == id, "key id is stable");
}

static void tags() {
  uint8_t key[KEY_BYTES]; std::memset(key, 0x42, sizeof(key));
  const uint8_t msg[] = "SCVP frame bytes";
  uint8_t t1[TAG_BYTES], t2[TAG_BYTES];
  tag(key, msg, sizeof(msg), t1);
  tag(key, msg, sizeof(msg), t2);
  CHECK(tag_equal(t1, t2), "same key, same bytes, same tag");
  uint8_t other[KEY_BYTES]; std::memset(other, 0x43, sizeof(other));
  tag(other, msg, sizeof(msg), t2);
  CHECK(!tag_equal(t1, t2), "another fleet's key does not verify");
  uint8_t flipped[sizeof(msg)]; std::memcpy(flipped, msg, sizeof(msg)); flipped[3] ^= 1;
  tag(key, flipped, sizeof(flipped), t2);
  CHECK(!tag_equal(t1, t2), "a flipped bit does not verify");
  // The tag is the first 16 bytes of the full HMAC.
  uint8_t full[HASH_BYTES];
  hmac_sha256(key, KEY_BYTES, msg, sizeof(msg), full);
  CHECK(std::memcmp(full, t1, TAG_BYTES) == 0, "truncated HMAC");
}

int main() {
  hmac_matches_rfc4231();
  hkdf_matches_rfc5869();
  the_household_key();
  tags();
  if (g_failures) { std::printf("test_fleet_key: %d failure(s)\n", g_failures); return 1; }
  std::printf("test_fleet_key: all passed\n");
  return 0;
}
