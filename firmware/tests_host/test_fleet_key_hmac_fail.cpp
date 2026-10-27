// Host test: the fleet key and the peer protocol refuse, not run open, when
// their HMAC fails (firmware/common/network/fleet_key.h and peer_protocol.h,
// sweep F350 review).
//
// fleet_key's unchecked HMAC zeroes its output when the primitive fails (on
// the device, mbedtls_md_hmac() on an allocation failure). Before this suite:
//   - hkdf_sha256() ignored both HMAC results and returned true, so
//     derive_from_wifi() handed peer_link an all-zero fleet key as a success,
//     and that key lasted until the next arm(); anyone can tag frames under
//     a zero key;
//   - tag() wrote a zero tag, and peer::decode() compared the frame's tag
//     against it, so a frame carrying sixteen zero bytes was accepted with no
//     key at all; peer::encode() sent a frame under that zero tag;
//   - key_id_hex() printed the id of a zero hash as the key's.
// This suite runs over hmac_fail_seam.h's primitive and is built twice
// (Makefile): over fleet_key.h's OpenSSL branch, and with -DARDUINO over its
// mbedTLS branch, the one the device ships. For each failure each library can
// report it holds that the HKDF and derive_from_wifi() refuse and wipe the
// output (a failure in the extract step or in the last expand round alike),
// that tag() says false, that encode() writes nothing, that decode()
// answers BadTag for the genuine frame and for a zero-tag one, and that
// key_id_hex() says false and writes "". Over the real primitive the same
// build derives RFC 5869's vector and round-trips a frame, so each refusal is
// the failure's. test_fleet_key.cpp and test_peer_protocol.cpp hold the
// working paths over the real HMAC.
//
// Build/run: make -C firmware/tests_host (links OpenSSL in both builds).

#include "../common/network/fleet_key.h"
#include "../common/network/peer_protocol.h"
#include "hmac_fail_seam.h"

#include <cstdio>
#include <cstring>

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

namespace fk = canary::net::fleet_key;
namespace peer = canary::net::peer;
using seam::Mode;
using seam::name_of;

static bool all_zero(const uint8_t* p, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= p[i];
  return d == 0;
}

static void fill_key(uint8_t (&k)[fk::KEY_BYTES], uint8_t v) { std::memset(k, v, sizeof(k)); }

static peer::Frame heartbeat_frame() {
  peer::Frame f;
  peer::Heartbeat hb;
  hb.caps = 0x0021;
  peer::put_heartbeat(f, hb);
  for (size_t i = 0; i < peer::FP_BYTES; i++) f.fp[i] = (uint8_t)(0x10 + i);
  f.uptime_s = 1234;
  f.seq = 7;
  return f;
}

// RFC 5869 A.1 (also test_fleet_key.cpp's vector).
static const uint8_t kIkm[22] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
static const uint8_t kSalt[13] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                  0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
static const uint8_t kInfo[10] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};
static const uint8_t kOkm[42] = {
    0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36,
    0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56,
    0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65};

// ── over the real primitive: the paths below can succeed ──────────────────
static void the_real_primitive_works() {
  seam::g_mode = Mode::Pass;
  const int before = seam::g_calls;
  uint8_t okm[sizeof(kOkm)];
  CHECK(fk::hkdf_sha256(kSalt, sizeof(kSalt), kIkm, sizeof(kIkm), kInfo, sizeof(kInfo), okm,
                        sizeof(okm)),
        "pass: the HKDF refused over a working HMAC");
  CHECK(std::memcmp(okm, kOkm, sizeof(kOkm)) == 0, "pass: the HKDF missed RFC 5869 A.1");

  uint8_t k[fk::KEY_BYTES];
  CHECK(fk::derive_from_wifi("HomeNet", "hunter2-secret", k), "pass: derive_from_wifi refused");
  CHECK(!all_zero(k, sizeof(k)), "pass: derive_from_wifi gave a zero key");

  uint8_t key[fk::KEY_BYTES];
  fill_key(key, 0x42);
  uint8_t buf[peer::MAX_FRAME];
  const size_t n = peer::encode(heartbeat_frame(), key, buf, sizeof(buf));
  CHECK(n == peer::HEADER_BYTES + peer::HEARTBEAT_PAYLOAD + peer::TAG_BYTES,
        "pass: encode wrote %zu bytes", n);
  peer::Frame got;
  CHECK(peer::decode(buf, n, key, got) == peer::Verdict::Ok, "pass: a genuine frame was refused");

  char id[9];
  CHECK(fk::key_id_hex(key, id), "pass: key_id_hex said it failed");
  CHECK(std::strlen(id) == 8, "pass: key id \"%s\" is not eight characters", id);
  CHECK(seam::g_calls > before, "pass: nothing reached the primitive under test");
}

// ── over each failure ──────────────────────────────────────────────────────
static void the_hkdf_refuses(Mode f) {
  seam::g_mode = f;
  const int before = seam::g_calls;
  uint8_t okm[sizeof(kOkm)];
  std::memset(okm, 0xEE, sizeof(okm));
  CHECK(!fk::hkdf_sha256(kSalt, sizeof(kSalt), kIkm, sizeof(kIkm), kInfo, sizeof(kInfo), okm,
                         sizeof(okm)),
        "%s: the HKDF returned true", name_of(f));
  CHECK(all_zero(okm, sizeof(okm)), "%s: the HKDF left output behind", name_of(f));

  // The bootstrap fleet key: refused (the caller leaves the peer protocol
  // off), never an all-zero key reported as derived.
  uint8_t k[fk::KEY_BYTES];
  std::memset(k, 0xEE, sizeof(k));
  CHECK(!fk::derive_from_wifi("HomeNet", "hunter2-secret", k),
        "%s: derive_from_wifi returned true (%s key)", name_of(f),
        all_zero(k, sizeof(k)) ? "an all-zero" : "a");
  CHECK(all_zero(k, sizeof(k)), "%s: derive_from_wifi left output behind", name_of(f));
  CHECK(seam::g_calls > before, "%s: the HKDF did not reach the primitive under test", name_of(f));
}

// A failure part-way through: 42 bytes of output are one extract and two
// expand rounds, and the first two run for real, so the first round has
// copied 32 bytes into okm before the last one fails. The refusal still wipes
// them.
static void the_hkdf_refuses_a_late_failure(Mode f) {
  seam::g_mode = f;
  seam::g_pass_first = 2;
  uint8_t okm[sizeof(kOkm)];
  std::memset(okm, 0xEE, sizeof(okm));
  CHECK(!fk::hkdf_sha256(kSalt, sizeof(kSalt), kIkm, sizeof(kIkm), kInfo, sizeof(kInfo), okm,
                         sizeof(okm)),
        "%s: the HKDF returned true when its last round failed", name_of(f));
  CHECK(all_zero(okm, sizeof(okm)), "%s: the HKDF left its first round's output behind (byte 0 is "
        "0x%02x)", name_of(f), okm[0]);
  CHECK(seam::g_pass_first == 0, "%s: the HKDF made fewer than two HMACs before the last",
        name_of(f));
  seam::g_pass_first = 0;
}

// A failure up front: the extract step fails and every expand round after it
// would run. Expanding a zeroed PRK gives a key anyone can compute, so the
// refusal must come from the extract's own result, not the last round's.
static void the_hkdf_refuses_an_early_failure(Mode f) {
  seam::g_mode = f;
  seam::g_fail_count = 1;
  uint8_t okm[sizeof(kOkm)];
  std::memset(okm, 0xEE, sizeof(okm));
  CHECK(!fk::hkdf_sha256(kSalt, sizeof(kSalt), kIkm, sizeof(kIkm), kInfo, sizeof(kInfo), okm,
                         sizeof(okm)),
        "%s: the HKDF returned true when its extract step failed", name_of(f));
  CHECK(all_zero(okm, sizeof(okm)), "%s: the HKDF expanded a PRK it never extracted (byte 0 is "
        "0x%02x)", name_of(f), okm[0]);
  CHECK(seam::g_fail_count == 0, "%s: the HKDF made no HMAC", name_of(f));
  seam::g_fail_count = -1;
}

static void the_tag_refuses(Mode f) {
  uint8_t key[fk::KEY_BYTES];
  fill_key(key, 0x42);

  // A genuine frame, tagged over the real primitive while it worked.
  seam::g_mode = Mode::Pass;
  uint8_t genuine[peer::MAX_FRAME];
  const size_t n = peer::encode(heartbeat_frame(), key, genuine, sizeof(genuine));
  CHECK(n > peer::TAG_BYTES, "pass: encode refused");
  if (n <= peer::TAG_BYTES) return;  // nothing to forge from
  // The forgery: the same frame with sixteen zero bytes for its tag.
  uint8_t forged[peer::MAX_FRAME];
  std::memcpy(forged, genuine, n);
  std::memset(forged + n - peer::TAG_BYTES, 0, peer::TAG_BYTES);

  seam::g_mode = f;
  const int before = seam::g_calls;
  uint8_t t[fk::TAG_BYTES];
  std::memset(t, 0xEE, sizeof(t));
  CHECK(!fk::tag(key, genuine, n - peer::TAG_BYTES, t), "%s: tag() said it ran", name_of(f));
  CHECK(all_zero(t, sizeof(t)), "%s: tag() left output behind", name_of(f));

  peer::Frame got;
  const peer::Verdict vf = peer::decode(forged, n, key, got);
  CHECK(vf == peer::Verdict::BadTag, "%s: a zero-tag frame decoded as %s", name_of(f),
        peer::verdict_label(vf));
  const peer::Verdict vg = peer::decode(genuine, n, key, got);
  CHECK(vg == peer::Verdict::BadTag, "%s: a frame the HMAC could not check decoded as %s",
        name_of(f), peer::verdict_label(vg));

  uint8_t out[peer::MAX_FRAME];
  std::memset(out, 0xEE, sizeof(out));
  const size_t m = peer::encode(heartbeat_frame(), key, out, sizeof(out));
  CHECK(m == 0, "%s: encode wrote a %zu-byte frame it could not tag", name_of(f), m);
  CHECK(all_zero(out, n), "%s: encode left a frame in the buffer", name_of(f));

  char id[9];
  std::memset(id, 'Z', sizeof(id));
  CHECK(!fk::key_id_hex(key, id), "%s: key_id_hex said it ran", name_of(f));
  CHECK(id[0] == '\0', "%s: key_id_hex wrote \"%.8s\"", name_of(f), id);
  CHECK(seam::g_calls > before, "%s: the tag did not reach the primitive under test", name_of(f));
}

int main() {
  the_real_primitive_works();
  for (Mode f : seam::kFailures) {
    the_hkdf_refuses(f);
    the_hkdf_refuses_a_late_failure(f);
    the_hkdf_refuses_an_early_failure(f);
    the_tag_refuses(f);
  }
  if (g_failures) {
    std::printf("test_fleet_key_hmac_fail (%s): %d failure(s)\n", seam::kBranch, g_failures);
    return 1;
  }
  std::printf("test_fleet_key_hmac_fail: OK (%s: a failed HMAC gives no fleet key, no tag, no "
              "frame and no key id, and a zero-tag frame is refused; the working HMAC derives "
              "RFC 5869 A.1 and round-trips a frame; %d primitive calls)\n",
              seam::kBranch, seam::g_calls);
  return 0;
}
