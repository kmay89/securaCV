/* Host tests for the first-boot identity key draw check (repo sweep F38):
 * firmware/common/identity/key_draw_check.h, and the two keygens that call
 * it with no NVS around them, run on their own code: the canary (PIO)
 * tree's crypto_generate_keypair() and secure_zero(), cut verbatim out of
 * securacv_crypto.cpp, and the canary-wap sketch's generate_keypair() and
 * secure_zero(), cut out of canary_wap.ino (cut_functions.awk; the Makefile
 * writes one .inc per tree), each compiled in a namespace of its own over
 * stand-ins for the RNG, the bootloader_random pair, Ed25519 and Serial.
 * (canary-sense's, canary-vision's and canary-sentinel's keygen stores the
 * key in the same function, so its flat draw runs in
 * test_witness_key_load_projects.cpp, over the fake NVS.)
 *
 * THE FAILURE THIS PREVENTS. Every keygen drew 32 bytes with
 * esp_fill_random() and used them as the identity key without looking: a
 * buffer the RNG never filled (the zeros a static buffer starts as) or a
 * stuck output became the unit's key for good, and every unit it happened to
 * named itself by the same fingerprint. Now a draw whose bytes are all equal
 * or whose popcount is outside [48, 208] is refused: the keygen answers
 * false with the buffer zeroed and derives no public key.
 *
 * What it pins:
 *   - is_flat(): every constant draw (all 256 byte values), a null draw,
 *     and a non-constant draw on each side of each bound (47 and 209 one
 *     bits refused, 48 and 208 passed); 200000 draws from a seeded PRNG
 *     pass;
 *   - the false-refusal odds the header states (about 2^-82.5, under
 *     2^-64), computed here from the header's own bounds;
 *   - both cut keygens: a good draw is kept (the public key derived from
 *     it, true); each flat draw (the buffer left as the RNG found it, all
 *     0xFF, a constant, 47 one bits) answers false, zeroes the buffer, leaves
 *     the public key unwritten and says so once; the draw runs inside the
 *     bootloader_random pair and the pair is closed on both paths;
 *   - the five keygen sources (argv): each asks is_flat() of its draw on the
 *     line after esp_fill_random() and before bootloader_random_disable(),
 *     refuses before it derives or stores a key, and its caller refuses on
 *     the false (provisioning fails in the caller's "Keypair generation
 *     failed" branch, which nothing reached before; the WAP's also refuses
 *     the boot's chain); both caller files must be given.
 *
 * What this does not prove: that a draw it passes is random (a fixed but
 * varied pattern passes), the hardware RNG, or anything on a device. The
 * ESP32 compiles are firmware.yml's; nothing here has run on a bench.
 *
 * Build & run: make -C firmware/tests_host run-key-draw-check
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"  // stubs/sha_fail: a Serial that keeps every line
#include "identity/key_draw_check.h"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using key_draw_check::KEY_DRAW_LEN;
using key_draw_check::KEY_DRAW_MAX_ONES;
using key_draw_check::KEY_DRAW_MIN_ONES;

// ── the stand-ins the cut keygens call ─────────────────────────────────────

enum class Rng { Good, Untouched, AllFF, Constant, LowOnes, AtMinOnes };
static Rng g_rng = Rng::Good;
static int g_draws = 0;
static int g_gate = 0;            // bootloader_random_enable() calls not yet disabled
static int g_draws_outside = 0;   // draws made with the gate closed
static int g_gate_enables = 0, g_gate_disables = 0;
static int g_derives = 0;

// A draw with exactly `ones` one bits, not every byte equal.
static void fill_with_ones(uint8_t* b, unsigned ones) {
  std::memset(b, 0, KEY_DRAW_LEN);
  for (unsigned i = 0; i < ones; i++) {
    const unsigned bit = (i * 37u) % 256u;  // 37 is odd, so 256 distinct bits
    b[bit / 8] |= (uint8_t)(1u << (bit % 8));
  }
}
static uint8_t good_byte(size_t i) { return (uint8_t)(0xC3 ^ (i * 11)); }

void bootloader_random_enable() {
  g_gate++;
  g_gate_enables++;
}
void bootloader_random_disable() {
  g_gate--;
  g_gate_disables++;
}
void esp_fill_random(void* buf, size_t len) {
  g_draws++;
  if (g_gate != 1) g_draws_outside++;
  uint8_t* b = static_cast<uint8_t*>(buf);
  CHECK(len == KEY_DRAW_LEN);
  switch (g_rng) {
    case Rng::Good:
      for (size_t i = 0; i < len; i++) b[i] = good_byte(i);
      break;
    case Rng::Untouched:
      break;  // the buffer as the caller handed it
    case Rng::AllFF:
      std::memset(b, 0xFF, len);
      break;
    case Rng::Constant:
      std::memset(b, 0x5A, len);
      break;
    case Rng::LowOnes:
      fill_with_ones(b, KEY_DRAW_MIN_ONES - 1);
      break;
    case Rng::AtMinOnes:
      fill_with_ones(b, KEY_DRAW_MIN_ONES);
      break;
  }
}
struct Ed25519 {
  static void derivePublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    g_derives++;
    for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)~priv[i];
  }
};

// ── each tree's cut, in a namespace of its own ──────────────────────────────

namespace flagship {
void secure_zero(void* p, size_t n);  // securacv_crypto.h's declaration; the cut defines it below its caller
#include "key_draw_check_flagship.inc"
}  // namespace flagship
namespace wap {
#include "key_draw_check_wap.inc"
}  // namespace wap

// ── part 1: the rule ────────────────────────────────────────────────────────

static void test_every_constant_draw_is_flat() {
  uint8_t d[KEY_DRAW_LEN];
  for (unsigned v = 0; v < 256; v++) {
    std::memset(d, (int)v, sizeof(d));
    CHECK(key_draw_check::is_flat(d));
  }
  CHECK(key_draw_check::is_flat(nullptr));
}

static void test_the_popcount_bounds_are_inclusive() {
  uint8_t d[KEY_DRAW_LEN];
  fill_with_ones(d, KEY_DRAW_MIN_ONES - 1);
  CHECK(key_draw_check::ones(d) == KEY_DRAW_MIN_ONES - 1);
  CHECK(key_draw_check::is_flat(d));
  fill_with_ones(d, KEY_DRAW_MIN_ONES);
  CHECK(key_draw_check::ones(d) == KEY_DRAW_MIN_ONES);
  CHECK(!key_draw_check::is_flat(d));
  fill_with_ones(d, KEY_DRAW_MAX_ONES);
  CHECK(key_draw_check::ones(d) == KEY_DRAW_MAX_ONES);
  CHECK(!key_draw_check::is_flat(d));
  fill_with_ones(d, KEY_DRAW_MAX_ONES + 1);
  CHECK(key_draw_check::ones(d) == KEY_DRAW_MAX_ONES + 1);
  CHECK(key_draw_check::is_flat(d));
  // One byte off a constant is not every byte equal, and passes when its
  // bit count is in bounds.
  std::memset(d, 0x0F, sizeof(d));  // 128 one bits, constant: flat
  CHECK(key_draw_check::is_flat(d));
  d[31] = 0x1F;
  CHECK(!key_draw_check::is_flat(d));
  // The bounds themselves: symmetric about 128, as the odds below assume.
  CHECK(KEY_DRAW_MIN_ONES + KEY_DRAW_MAX_ONES == 256);
}

static void test_random_draws_pass() {
  std::mt19937_64 rng(0x5EC0AC7u);
  uint8_t d[KEY_DRAW_LEN];
  int refused = 0;
  for (int n = 0; n < 200000; n++) {
    for (size_t i = 0; i < KEY_DRAW_LEN; i += 8) {
      const uint64_t w = rng();
      std::memcpy(d + i, &w, 8);
    }
    if (key_draw_check::is_flat(d)) refused++;
  }
  CHECK(refused == 0);
}

// P(refuse) for a uniform 256-bit draw: both popcount tails (the binomial
// is symmetric) plus the 256 constant draws, by the union bound. Computed
// in log space, term by term, from the header's own bounds.
static void test_the_false_refusal_odds_are_what_the_header_says() {
  long double tail = 0.0L;
  for (unsigned k = 0; k < KEY_DRAW_MIN_ONES; k++) {
    const long double log_c = std::lgamma(257.0L) - std::lgamma((long double)k + 1.0L) -
                              std::lgamma((long double)(256 - k) + 1.0L);
    tail += std::exp(log_c - 256.0L * std::log(2.0L));
  }
  const long double p = 2.0L * tail + std::ldexp(1.0L, -248);
  const long double log2p = std::log2(p);
  std::printf("  false-refusal odds for a uniform draw: %.3Le = 2^%.2Lf\n", p, log2p);
  CHECK(p < std::ldexp(1.0L, -64));          // the bound the check was set to
  CHECK(log2p > -83.0L && log2p < -82.0L);   // the header's "about 2^-82.5"
}

// ── part 2: the two keygens, on their own code ──────────────────────────────

struct Keygen {
  const char* name;
  bool (*generate)(uint8_t priv[32], uint8_t pub[32]);
  const char* refusal_line;
};
static const Keygen kKeygens[] = {
    {"canary (PIO) crypto_generate_keypair", flagship::crypto_generate_keypair,
     "[!!] Identity key draw came out flat"},
    {"canary-wap generate_keypair", wap::generate_keypair, "[!!] Identity key draw came out flat"},
};

static void reset_world(Rng rng) {
  g_rng = rng;
  g_draws = g_gate = g_draws_outside = g_gate_enables = g_gate_disables = g_derives = 0;
  Serial.lines.clear();
}

static bool said(const char* needle) {
  int n = 0;
  for (const std::string& l : Serial.lines)
    if (l.find(needle) != std::string::npos) n++;
  return n == 1;
}

static void test_a_good_draw_is_kept(const Keygen& k) {
  reset_world(Rng::Good);
  uint8_t priv[32] = {0}, pub[32];
  std::memset(pub, 0x77, sizeof(pub));
  CHECK(k.generate(priv, pub));
  for (size_t i = 0; i < 32; i++) {
    CHECK(priv[i] == good_byte(i));
    CHECK(pub[i] == (uint8_t)~good_byte(i));
  }
  CHECK(g_draws == 1 && g_draws_outside == 0);
  CHECK(g_gate == 0 && g_gate_enables == 1 && g_gate_disables == 1);
  CHECK(g_derives == 1);
  CHECK(Serial.lines.empty());
  // A draw exactly at the lower bound is a key, not a refusal.
  reset_world(Rng::AtMinOnes);
  std::memset(priv, 0, sizeof(priv));
  CHECK(k.generate(priv, pub));
  CHECK(key_draw_check::ones(priv) == KEY_DRAW_MIN_ONES);
}

static void test_a_flat_draw_is_refused(const Keygen& k) {
  for (Rng rng : {Rng::Untouched, Rng::AllFF, Rng::Constant, Rng::LowOnes}) {
    reset_world(rng);
    uint8_t priv[32] = {0}, pub[32];
    std::memset(pub, 0x77, sizeof(pub));
    const bool kept = k.generate(priv, pub);
    if (kept) std::printf("  %s kept a flat draw (case %d)\n", k.name, (int)rng);
    CHECK(!kept);
    uint8_t any = 0;
    for (uint8_t b : priv) any |= b;
    CHECK(any == 0);                            // the buffer zeroed
    for (uint8_t b : pub) CHECK(b == 0x77);     // no public key derived
    CHECK(g_derives == 0);
    CHECK(g_draws == 1 && g_draws_outside == 0);
    CHECK(g_gate == 0 && g_gate_enables == 1 && g_gate_disables == 1);  // the pair closed
    CHECK(said(k.refusal_line));
  }
}

// ── part 3: the five keygen sources ─────────────────────────────────────────

static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
static std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end + 3 - at);
}
static size_t count_of(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) n++;
  return n;
}

struct Source {
  const char* path_tail;
  const char* keygen_head;  // the keygen's signature line
  const char* buffer;       // the buffer the draw fills
  const char* after;        // what must come only after the refusal
};
static const Source kSources[] = {
    {"canary/lib/securacv_crypto/src/securacv_crypto.cpp",
     "\nbool crypto_generate_keypair(uint8_t priv[32], uint8_t pub[32]) {\n", "priv",
     "Ed25519::derivePublicKey("},
    {"projects/canary-sense/src/witness.cpp", "\nbool load_or_generate_keypair() {\n", "s_priv",
     "prefs.putBytes(KEY_PRIV"},
    {"projects/canary-vision/src/witness.cpp", "\nbool load_or_generate_keypair() {\n", "s_priv",
     "prefs.putBytes(KEY_PRIV"},
    {"projects/canary-sentinel/src/witness.cpp", "\nbool load_or_generate_keypair() {\n", "s_priv",
     "prefs.putBytes(KEY_PRIV"},
    {"projects/canary-wap/arduino/canary_wap/canary_wap.ino",
     "\nstatic bool generate_keypair(uint8_t priv[32], uint8_t pub[32]) {\n", "priv",
     "Ed25519::derivePublicKey("},
};

static void test_the_five_keygens_ask_inside_the_pair(const std::vector<std::string>& files) {
  int seen = 0;
  for (const Source& s : kSources) {
    std::string path;
    for (const std::string& f : files)
      if (ends_with(f, s.path_tail)) path = f;
    if (path.empty()) {
      std::printf("  not given: %s\n", s.path_tail);
      CHECK(false);
      continue;
    }
    seen++;
    const std::string src = read_file(path);
    const std::string body = body_of(src, s.keygen_head);
    CHECK(!body.empty());
    const std::string draw = std::string("  esp_fill_random(") + s.buffer;
    const std::string ask = std::string("\n  const bool flat = key_draw_check::is_flat(") + s.buffer + ");\n";
    const size_t enable = body.find("\n  bootloader_random_enable();\n");
    const size_t fill = body.find(draw);
    const size_t asked = body.find(ask);
    const size_t disable = body.find("\n  bootloader_random_disable();\n");
    const size_t refuse = body.find("\n  if (flat) {\n");
    const size_t used = body.find(s.after);
    const bool ordered = enable != std::string::npos && fill != std::string::npos &&
                         asked != std::string::npos && disable != std::string::npos &&
                         refuse != std::string::npos && used != std::string::npos &&
                         enable < fill && fill < asked && asked < disable && disable < refuse &&
                         refuse < used;
    if (!ordered) std::printf("  %s: enable/draw/ask/disable/refuse/use out of order\n", s.path_tail);
    CHECK(ordered);
    // The ask is the line after the draw, and the refusal returns false
    // with the buffer scrubbed.
    if (fill != std::string::npos && asked != std::string::npos) {
      CHECK(body.find('\n', fill) == asked);
    }
    if (refuse != std::string::npos && used != std::string::npos && refuse < used) {
      const std::string branch = body.substr(refuse, used - refuse);
      CHECK(branch.find(std::string("secure_zero(") + s.buffer) != std::string::npos);
      CHECK(branch.find("return false;") != std::string::npos);
    }
    CHECK(count_of(body, "key_draw_check::is_flat(") == 1);
    CHECK(count_of(body, "esp_fill_random(") == 1);
  }
  CHECK(seen == 5);

  // Each caller refuses on the false, in the "Keypair generation failed"
  // branch nothing reached before: the canary's and the WAP's
  // establish_identity_key() return false before anything stores a key (the
  // WAP's refusing the boot's chain too: test_wap_first_boot_nvs.cpp runs
  // it), and each project's init() returns false before the signer sees one
  // (the WITKEY suite pins init()). Both caller files must be given.
  int callers_seen = 0;
  for (const std::string& f : files) {
    if (ends_with(f, "canary/lib/securacv_witness/src/securacv_witness.cpp")) {
      callers_seen++;
      const std::string est = body_of(read_file(f), "\nstatic bool establish_identity_key() {\n");
      const size_t gen = est.find("  if (!crypto_generate_keypair(g_device.privkey, g_device.pubkey)) {\n");
      const size_t store = est.find("nvs_store_key(");
      CHECK(gen != std::string::npos && store != std::string::npos && gen < store);
      if (gen != std::string::npos && store != std::string::npos && gen < store)
        CHECK(est.substr(gen, store - gen).find("    return false;\n") != std::string::npos);
    }
    if (ends_with(f, "projects/canary-wap/arduino/canary_wap/canary_wap.ino")) {
      callers_seen++;
      const std::string est = body_of(read_file(f), "\nstatic bool establish_identity_key() {\n");
      const size_t gen = est.find("  if (!generate_keypair(g_device.privkey, g_device.pubkey)) {\n");
      const size_t store = est.find("nvs_store_key(");
      CHECK(gen != std::string::npos && store != std::string::npos && gen < store);
      if (gen != std::string::npos && store != std::string::npos && gen < store) {
        const std::string branch = est.substr(gen, store - gen);
        CHECK(branch.find("    return false;\n") != std::string::npos);
        CHECK(branch.find("    g_device.chain_refused = true;\n") != std::string::npos);
      }
    }
  }
  CHECK(callers_seen == 2);
}

int main(int argc, char** argv) {
  std::vector<std::string> files(argv + 1, argv + argc);
  test_every_constant_draw_is_flat();
  test_the_popcount_bounds_are_inclusive();
  test_random_draws_pass();
  test_the_false_refusal_odds_are_what_the_header_says();
  for (const Keygen& k : kKeygens) {
    test_a_good_draw_is_kept(k);
    test_a_flat_draw_is_refused(k);
  }
  test_the_five_keygens_ask_inside_the_pair(files);
  if (g_failures) {
    std::printf("test_key_draw_check: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_key_draw_check: all %d checks passed\n", g_checks);
  return 0;
}
