/* Host tests for witness_page.h — the GET /api/v1/witness page contract
 * (spec/witness_api_v1.md): the `?last=N` parser, the RAM ring, the coarse
 * (ten-minute, Invariant III) timestamp anchoring, the ISO-8601 writer,
 * and — the point — a byte-exact render of the shared fixture
 * spec/fixtures/witness_page_v1.json from records this test rebuilds with
 * OpenSSL (the firmware's own domain-separated chain construction, real
 * Ed25519 over the raw chain hash from the fixture's TEST-ONLY seed). The
 * derived public key must equal PUBKEY_HEX, the constant the iPhone app's
 * WitnessPageFixtureTests pins, so one fixture proves both ends.
 *
 * Build & run (CI: firmware.yml host tests; the Makefile here runs it too):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_witness_page.cpp \
 *       -lcrypto -o /tmp/test_witness_page \
 *   && /tmp/test_witness_page spec/fixtures/witness_page_v1.json
 *
 * The fixture path is argv[1]; without it the test looks for the file
 * relative to the repo root and to this directory.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "witness_page.h"

using namespace witness_page;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

// ── the fixture's constants (spec/fixtures/gen_witness_page_v1.py) ─────────
static const char     FIXTURE_SEED[]      = "fixture-key-for-witness-page-v1!";  // 32 bytes, test-only
static const char     PUBKEY_HEX[]        = "22b7279917b3a47068f4d17f0ee3182b5fef9035592765f9b55e4eb97d167e1c";
static const char     FIXTURE_DEVICE_ID[] = "canary-fixture-0001";
static const uint32_t FIXTURE_BUCKET_MS   = 5000;
static const uint32_t FIXTURE_NOW_MS      = 700000;
static const uint32_t FIXTURE_NOW_EPOCH_S = 1788864000u;  // 2026-09-08T10:40:00Z

struct FixtureRow { uint32_t seq; uint32_t tb; uint8_t type; const char* payload; };
static const FixtureRow FIXTURE_ROWS[] = {
    {1, 1,   0, "boot:canary-fixture-0001"},
    {2, 130, 1, "event:presence_changed"},
    {3, 131, 2, "tamper:enclosure_tamper"},
    {4, 132, 3, "state:NOFIX->ACQRD"},
};

// ── crypto helpers (OpenSSL stands in for mbedtls / the Arduino Ed25519) ───
static void sha256_domain(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, domain, strlen(domain));
  const uint8_t sep = 0x00;
  SHA256_Update(&ctx, &sep, 1);
  if (data && n) SHA256_Update(&ctx, data, n);
  SHA256_Final(out, &ctx);
}

// canary_wap.ino compute_chain_hash, byte for byte.
static void chain_hash(const uint8_t prev[32], const uint8_t ph[32], uint32_t seq,
                       uint32_t tb, uint8_t out[32]) {
  uint8_t buf[72];
  memcpy(buf, prev, 32);
  memcpy(buf + 32, ph, 32);
  buf[64] = (uint8_t)(seq >> 24); buf[65] = (uint8_t)(seq >> 16);
  buf[66] = (uint8_t)(seq >> 8);  buf[67] = (uint8_t)seq;
  buf[68] = (uint8_t)(tb >> 24);  buf[69] = (uint8_t)(tb >> 16);
  buf[70] = (uint8_t)(tb >> 8);   buf[71] = (uint8_t)tb;
  sha256_domain("securacv:fw:chain:v1", buf, sizeof(buf), out);
}

struct Ed25519Key {
  EVP_PKEY* pkey = nullptr;
  uint8_t pub[32];
  ~Ed25519Key() { if (pkey) EVP_PKEY_free(pkey); }
};

static bool key_from_seed(Ed25519Key& k, const char* seed) {
  k.pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                        (const unsigned char*)seed, 32);
  if (!k.pkey) return false;
  size_t len = 32;
  return EVP_PKEY_get_raw_public_key(k.pkey, k.pub, &len) == 1 && len == 32;
}

static bool sign(const Ed25519Key& k, const uint8_t* msg, size_t len, uint8_t sig[64]) {
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  size_t sig_len = 64;
  const bool ok = EVP_DigestSignInit(md, nullptr, nullptr, nullptr, k.pkey) == 1 &&
                  EVP_DigestSign(md, sig, &sig_len, msg, len) == 1 && sig_len == 64;
  EVP_MD_CTX_free(md);
  return ok;
}

static bool verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
  EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
  if (!pk) return false;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok = EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pk) == 1 &&
                  EVP_DigestVerify(md, sig, 64, msg, len) == 1;
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(pk);
  return ok;
}

static bool hex_decode(uint8_t* out, const char* hex, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(hex + 2 * i, "%02x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

// ── 1. ?last=N ──────────────────────────────────────────────────────────────
static void test_parse_last() {
  CHECK(parse_last(nullptr) == DEFAULT_LAST);
  CHECK(parse_last("") == DEFAULT_LAST);
  CHECK(parse_last("last=5") == 5);
  CHECK(parse_last("x=1&last=7") == 7);
  CHECK(parse_last("last=7&x=1") == 7);
  CHECK(parse_last("last=0") == 1);                // floor
  CHECK(parse_last("last=100") == MAX_LAST);
  CHECK(parse_last("last=101") == MAX_LAST);       // ceiling
  CHECK(parse_last("last=99999999999999") == MAX_LAST);  // no overflow wrap
  CHECK(parse_last("last=abc") == DEFAULT_LAST);
  CHECK(parse_last("last=") == DEFAULT_LAST);
  CHECK(parse_last("last=5x") == DEFAULT_LAST);    // trailing junk is malformed, not 5
  CHECK(parse_last("blast=3") == DEFAULT_LAST);    // must not match inside another key
  CHECK(parse_last("x=blast=3") == DEFAULT_LAST);
}

// ── 2. the ring ─────────────────────────────────────────────────────────────
static Record make_record(uint32_t seq) {
  Record r;
  memset(&r, 0, sizeof(r));
  r.seq = seq;
  r.time_bucket = seq * 10;
  r.bucket_ms = 5000;
  r.type = (uint8_t)(seq % 4);
  return r;
}

static void test_ring() {
  static Ring ring;  // zero-initialized storage is an empty ring
  const Record* rows[RING_CAP];
  CHECK(ring.count == 0 && ring.newest(5, rows) == 0);

  for (uint32_t s = 1; s <= 20; s++) ring.push(make_record(s));
  CHECK(ring.count == RING_CAP);

  // The newest five, oldest first.
  CHECK(ring.newest(5, rows) == 5);
  for (size_t i = 0; i < 5; i++) CHECK(rows[i]->seq == 16 + i);

  // More than held → clamped to the ring, still oldest first.
  CHECK(ring.newest(100, rows) == RING_CAP);
  for (size_t i = 0; i < RING_CAP; i++) CHECK(rows[i]->seq == 5 + i);

  // Exactly one: the head.
  CHECK(ring.newest(1, rows) == 1 && rows[0]->seq == 20);

  // snapshot(): the same selection, copied out — what the sketch renders
  // from, under its lock, so a concurrent push cannot tear a row.
  Record copies[RING_CAP];
  CHECK(ring.snapshot(5, copies) == 5);
  for (size_t i = 0; i < 5; i++) CHECK(copies[i].seq == 16 + i);
  CHECK(ring.snapshot(100, copies) == RING_CAP);
  CHECK(copies[0].seq == 5 && copies[RING_CAP - 1].seq == 20);
  CHECK(ring.snapshot(0, copies) == 0);

  ring.clear();
  CHECK(ring.count == 0 && ring.newest(1, rows) == 0);
}

// ── 3. JSON escaping of the device id ───────────────────────────────────────
static void test_json_escape() {
  char out[32];
  CHECK(json_escape(out, sizeof(out), "canary-a1b2") && strcmp(out, "canary-a1b2") == 0);
  CHECK(json_escape(out, sizeof(out), "a\"b\\c") && strcmp(out, "a\\\"b\\\\c") == 0);
  CHECK(json_escape(out, sizeof(out), "x\ny\tz") && strcmp(out, "xyz") == 0);  // controls dropped
  CHECK(json_escape(out, sizeof(out), nullptr) && out[0] == '\0');
  char tiny[4];
  CHECK(!json_escape(tiny, sizeof(tiny), "abcd"));       // no room for the NUL
  CHECK(json_escape(tiny, sizeof(tiny), "abc") && strcmp(tiny, "abc") == 0);
  CHECK(!json_escape(tiny, sizeof(tiny), "ab\""));       // the escape needs two bytes
}

// ── 4. ISO-8601 from a Unix time, integer math only ─────────────────────────
static void test_iso8601() {
  char iso[21];
  iso8601_utc(iso, 0u);           CHECK(strcmp(iso, "1970-01-01T00:00:00Z") == 0);
  iso8601_utc(iso, 951782400u);   CHECK(strcmp(iso, "2000-02-29T00:00:00Z") == 0);  // century leap day
  iso8601_utc(iso, 1709164800u);  CHECK(strcmp(iso, "2024-02-29T00:00:00Z") == 0);
  iso8601_utc(iso, 1709251199u);  CHECK(strcmp(iso, "2024-02-29T23:59:59Z") == 0);
  iso8601_utc(iso, 1788864000u);  CHECK(strcmp(iso, "2026-09-08T10:40:00Z") == 0);
  iso8601_utc(iso, 4102444800u);  CHECK(strcmp(iso, "2100-01-01T00:00:00Z") == 0);
  iso8601_utc(iso, 4294967295u);  CHECK(strcmp(iso, "2106-02-07T06:28:15Z") == 0);  // u32 end
}

// ── 5. coarse anchoring (Invariant III) ─────────────────────────────────────
static void test_record_epoch() {
  Record r = make_record(1);
  r.time_bucket = 130;  // start_ms = 650 000
  r.bucket_ms = 5000;
  uint32_t epoch = 0;

  // No believable clock → no timestamp at all, never a 1970 date.
  Context noclock = {"dev", 1, 700000, 12345};
  CHECK(!record_epoch(noclock, r, &epoch));
  Context boot_epoch = {"dev", 1, 700000, 0};
  CHECK(!record_epoch(boot_epoch, r, &epoch));

  // With a clock: bucket start = now - (700 000 - 650 000) ms = now - 50 s,
  // floored to the ten-minute grain. 1788864000 is 10:40:00Z exactly, so
  // 50 s earlier floors to 10:30:00Z.
  Context clock = {"dev", 1, 700000, 1788864000u};
  CHECK(record_epoch(clock, r, &epoch) && epoch == 1788863400u);
  CHECK(epoch % COARSE_S == 0);

  // A record from 695 s ago (bucket 1) floors one grain further back.
  Record early = make_record(1);
  early.time_bucket = 1; early.bucket_ms = 5000;
  CHECK(record_epoch(clock, early, &epoch) && epoch == 1788862800u);  // 10:20:00Z

  // millis() wrap: a record made just before the 32-bit wrap, read just
  // after it, still ages by the small difference.
  Context wrapped = {"dev", 1, 5000, 1788864000u};
  Record prewrap = make_record(1);
  prewrap.time_bucket = 4294960; prewrap.bucket_ms = 1000;  // start_ms = 4 294 960 000
  CHECK(record_epoch(wrapped, prewrap, &epoch) && epoch == 1788863400u);  // ~12 s old → 10:30 floor

  // The oldest a record can be under a 32-bit millis() is ~49.7 days, which
  // never exceeds a believable epoch — so the age > epoch guard in
  // record_epoch is defensive only. A 46-day-old record still anchors and
  // floors: 1700000000 - 4000000 = 1696000000 → floor 600 → 1695999600.
  Context long_uptime = {"dev", 1, 4000000000u, 1700000000u};
  Record old_rec = make_record(1);
  old_rec.time_bucket = 0; old_rec.bucket_ms = 5000;
  CHECK(record_epoch(long_uptime, old_rec, &epoch) && epoch == 1695999600u);
}

// ── 6. the shared fixture, byte for byte ────────────────────────────────────
static std::string read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return std::string();
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static std::string load_fixture(int argc, char** argv) {
  if (argc > 1) return read_file(argv[1]);
  const char* candidates[] = {
      "spec/fixtures/witness_page_v1.json",                 // repo root (CI)
      "../../../../spec/fixtures/witness_page_v1.json",     // tests_host/ (make)
      "../../../spec/fixtures/witness_page_v1.json",
  };
  for (const char* c : candidates) {
    std::string s = read_file(c);
    if (!s.empty()) return s;
  }
  return std::string();
}

// Rebuild the fixture's ring exactly as the firmware would have built it.
static bool build_fixture_ring(Ring& ring, const Ed25519Key& key) {
  ring.clear();
  uint8_t prev[32];
  sha256_domain("securacv:genesis:v1", (const uint8_t*)FIXTURE_DEVICE_ID,
                strlen(FIXTURE_DEVICE_ID), prev);
  for (const FixtureRow& row : FIXTURE_ROWS) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.seq = row.seq;
    r.time_bucket = row.tb;
    r.bucket_ms = FIXTURE_BUCKET_MS;
    r.type = row.type;
    sha256_domain("securacv:fw:payload:v1", (const uint8_t*)row.payload,
                  strlen(row.payload), r.payload_hash);
    memcpy(r.prev_hash, prev, 32);
    chain_hash(r.prev_hash, r.payload_hash, r.seq, r.time_bucket, r.chain_hash);
    if (!sign(key, r.chain_hash, 32, r.signature)) return false;
    ring.push(r);
    memcpy(prev, r.chain_hash, 32);
  }
  return true;
}

static void test_fixture(int argc, char** argv) {
  Ed25519Key key;
  CHECK(key_from_seed(key, FIXTURE_SEED));

  // The Swift test pins PUBKEY_HEX; it must be THIS seed's public key.
  uint8_t pinned[32];
  CHECK(hex_decode(pinned, PUBKEY_HEX, 32));
  CHECK(memcmp(pinned, key.pub, 32) == 0);

  static Ring ring;
  CHECK(build_fixture_ring(ring, key));
  CHECK(ring.count == 4);

  // Every record's signature verifies under the pinned key over the RAW
  // 32-byte chain hash — the WAP scheme (never over the hex string).
  const Record* rows[RING_CAP];
  CHECK(ring.newest(4, rows) == 4);
  for (size_t i = 0; i < 4; i++) {
    CHECK(verify(pinned, rows[i]->chain_hash, 32, rows[i]->signature));
    char hex[65];
    to_hex(hex, rows[i]->chain_hash, 32);
    CHECK(!verify(pinned, (const uint8_t*)hex, 64, rows[i]->signature));
    // ...and chains: prev of i+1 is hash of i.
    if (i + 1 < 4) CHECK(memcmp(rows[i + 1]->prev_hash, rows[i]->chain_hash, 32) == 0);
  }

  Context ctx = {FIXTURE_DEVICE_ID, 4, FIXTURE_NOW_MS, FIXTURE_NOW_EPOCH_S};
  static char page[8192];
  const size_t n = page_build(page, sizeof(page), ring, DEFAULT_LAST, ctx);
  CHECK(n > 0 && n == strlen(page));

  const std::string fixture = load_fixture(argc, argv);
  if (fixture.empty()) {
    std::printf("FAIL: fixture spec/fixtures/witness_page_v1.json not found "
                "(pass its path as argv[1])\n");
    g_failures++;
    return;
  }
  if (fixture != std::string(page, n)) {
    std::printf("FAIL: rendered page differs from the fixture\n--- rendered ---\n%s\n"
                "--- fixture ---\n%s\n", page, fixture.c_str());
    g_failures++;
  }

  // The streamed form (what handle_witness_v1 sends chunk by chunk) is the
  // same bytes as the one-buffer render.
  std::string streamed;
  char piece[RECORD_MAX];
  size_t w = header_build(piece, sizeof(piece), ctx);
  CHECK(w > 0);
  streamed.append(piece, w);
  for (size_t i = 0; i < 4; i++) {
    w = record_build(piece, sizeof(piece), *rows[i], ctx, i == 0);
    CHECK(w > 0);
    streamed.append(piece, w);
  }
  w = footer_build(piece, sizeof(piece));
  CHECK(w > 0);
  streamed.append(piece, w);
  CHECK(streamed == fixture);

  // Field spot checks the Swift side also makes.
  CHECK(strstr(page, "\"chain_format\":\"wap_v1\"") != nullptr);
  CHECK(strstr(page, "\"event_type\":\"tamper_detected\"") != nullptr);
  CHECK(strstr(page, "\"timestamp\":\"2026-09-08T10:20:00Z\"") != nullptr);
  CHECK(strstr(page, "\"timestamp\":\"2026-09-08T10:30:00Z\"") != nullptr);
  CHECK(strstr(page, "\"verified\"") == nullptr);   // the self-check never rides the wire
  CHECK(strstr(page, "\"total\":4") != nullptr);

  // `last` smaller than the ring: only the newest two, and the first one
  // carries no leading comma.
  const size_t n2 = page_build(page, sizeof(page), ring, 2, ctx);
  CHECK(n2 > 0);
  CHECK(strstr(page, "\"records\":[{\"seq\":3,") != nullptr);
  CHECK(strstr(page, "\"seq\":4,") != nullptr);
  CHECK(strstr(page, "\"seq\":1,") == nullptr);
  CHECK(strstr(page, "\"seq\":2,") == nullptr);

  // No believable clock: the bucket still rides (it is hashed), the
  // timestamp and time_source do not — and nothing says 1970.
  Context noclock = {FIXTURE_DEVICE_ID, 4, FIXTURE_NOW_MS, 0};
  const size_t n3 = page_build(page, sizeof(page), ring, DEFAULT_LAST, noclock);
  CHECK(n3 > 0);
  CHECK(strstr(page, "\"timestamp\"") == nullptr);
  CHECK(strstr(page, "\"time_source\"") == nullptr);
  CHECK(strstr(page, "1970") == nullptr);
  CHECK(strstr(page, "\"time_bucket\":130,\"time_bucket_ms\":5000") != nullptr);
  CHECK(strstr(page, "\"zone\":\"\",\"signature\":\"") != nullptr);

  // An empty ring is a valid, empty page.
  static Ring empty;
  const size_t n4 = page_build(page, sizeof(page), empty, DEFAULT_LAST, ctx);
  CHECK(n4 > 0);
  CHECK(strstr(page, "\"records\":[]}") != nullptr);
  CHECK(page[n4 - 1] == '}');

  // A buffer too small for the page reports 0, never a truncated page.
  char small[100];
  CHECK(page_build(small, sizeof(small), ring, DEFAULT_LAST, ctx) == 0);
  char header_only[200];
  CHECK(page_build(header_only, sizeof(header_only), ring, DEFAULT_LAST, ctx) == 0);
}

int main(int argc, char** argv) {
  test_parse_last();
  test_ring();
  test_json_escape();
  test_iso8601();
  test_record_epoch();
  test_fixture(argc, argv);
  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("ALL witness_page TESTS PASSED\n");
  return 0;
}
