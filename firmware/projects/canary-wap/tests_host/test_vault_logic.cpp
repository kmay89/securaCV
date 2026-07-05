/* Host tests for vault_logic.h — the sealed-snapshot vault's pure decisions
 * and byte-exact formats. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_vault_logic.cpp \
 *       -o /tmp/test_vault_logic && /tmp/test_vault_logic
 *
 * The GOLDEN_HEADER_HEX constant below is shared verbatim with
 * tools/test_unseal_snapshot.py — the python unlock tool parses the same
 * bytes, pinning the .svlt layout across both languages.
 */

#include <cstdio>
#include <cstring>

#include "vault_logic.h"

using namespace vault_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

/* Deterministic fixture header: trigger=T3(1), bucket=87,
 * key_id = 01..08, ephemeral = A0..BF, nonce = C0..CB, ct_len=0x0001F400
 * (128000). Byte-exact hex of header_build() output — if the layout ever
 * shifts, BOTH this test and tools/test_unseal_snapshot.py fail. */
static const char GOLDEN_HEADER_HEX[] =
    "53564c54"  /* "SVLT" */
    "01"        /* version */
    "01"        /* trigger t3 */
    "57"        /* bucket 87 */
    "00"        /* reserved */
    "0102030405060708"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacb"
    "00f40100"; /* 128000 LE */

static void fill_fixture(SealHeader* h) {
  memset(h, 0, sizeof(*h));
  h->trigger     = (uint8_t)Trigger::T3_SMOKE;
  h->time_bucket = 87;
  for (size_t i = 0; i < KEY_ID_SIZE; i++) h->key_id[i] = (uint8_t)(i + 1);
  for (size_t i = 0; i < PUBKEY_SIZE; i++) h->ephemeral_pub[i] = (uint8_t)(0xA0 + i);
  for (size_t i = 0; i < NONCE_SIZE; i++)  h->nonce[i] = (uint8_t)(0xC0 + i);
  h->ct_len = 128000;
}

static void hex_of(const uint8_t* buf, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) std::sprintf(out + 2 * i, "%02x", buf[i]);
}

static void test_header_golden_and_roundtrip() {
  SealHeader h;
  fill_fixture(&h);

  uint8_t raw[HEADER_SIZE];
  header_build(h, raw);

  char hex[HEADER_SIZE * 2 + 1] = {0};
  hex_of(raw, HEADER_SIZE, hex);
  CHECK(strlen(GOLDEN_HEADER_HEX) == HEADER_SIZE * 2);
  CHECK(strcmp(hex, GOLDEN_HEADER_HEX) == 0);

  SealHeader back;
  CHECK(header_parse(raw, &back));
  CHECK(back.trigger == h.trigger);
  CHECK(back.time_bucket == h.time_bucket);
  CHECK(memcmp(back.key_id, h.key_id, KEY_ID_SIZE) == 0);
  CHECK(memcmp(back.ephemeral_pub, h.ephemeral_pub, PUBKEY_SIZE) == 0);
  CHECK(memcmp(back.nonce, h.nonce, NONCE_SIZE) == 0);
  CHECK(back.ct_len == h.ct_len);
}

static void test_header_rejects_malformed() {
  SealHeader h;
  fill_fixture(&h);
  uint8_t raw[HEADER_SIZE];
  SealHeader out;

  header_build(h, raw); raw[0] = 'X';                    /* bad magic */
  CHECK(!header_parse(raw, &out));
  header_build(h, raw); raw[4] = 2;                      /* bad version */
  CHECK(!header_parse(raw, &out));
  header_build(h, raw); raw[5] = 7;                      /* unknown trigger */
  CHECK(!header_parse(raw, &out));
  header_build(h, raw); raw[6] = 144;                    /* bucket out of range */
  CHECK(!header_parse(raw, &out));
  h.ct_len = 0;               header_build(h, raw);      /* zero length */
  CHECK(!header_parse(raw, &out));
  h.ct_len = MAX_CIPHERTEXT + 1; header_build(h, raw);   /* oversized */
  CHECK(!header_parse(raw, &out));
  h.ct_len = MAX_CIPHERTEXT;  header_build(h, raw);      /* boundary ok */
  CHECK(header_parse(raw, &out));
}

static void test_capture_decision() {
  VaultConfig cfg{true, false, true, 60};  /* t3 + glass on, t4 off */

  /* Args: (trigger, cfg, has_pubkey, sd_ok, camera_ok, qr_active,
   *        worker_busy, now, last, has_last) */

  /* Happy path. */
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         false, 100000, 0, false) == Decision::CAPTURE);

  /* THE INVARIANT: with no registered public key, NOTHING is ever
   * captured — not even TEST. The vault is write-only escrow; no
   * recipient key means no capture, full stop. */
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, false, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_NO_KEY);
  CHECK(capture_decision(Trigger::TEST, cfg, false, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_NO_KEY);

  /* Hard preconditions, in contract order. */
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, false, true, false,
                         false, 100000, 0, false) == Decision::SKIP_NO_SD);
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, false, false,
                         false, 100000, 0, false) == Decision::SKIP_NO_CAMERA);
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, true,
                         false, 100000, 0, false) == Decision::SKIP_QR_BUSY);
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         true, 100000, 0, false) == Decision::SKIP_WORKER_BUSY);

  /* Per-trigger opt-in: t4 is off in this config. */
  CHECK(capture_decision(Trigger::T4_CO, cfg, true, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_DISABLED);
  CHECK(capture_decision(Trigger::GLASS, cfg, true, true, true, false,
                         false, 100000, 0, false) == Decision::CAPTURE);

  /* All-off config (the factory default): every alarm trigger skips. */
  VaultConfig off{false, false, false, 60};
  CHECK(capture_decision(Trigger::T3_SMOKE, off, true, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_DISABLED);

  /* TEST bypasses opt-in and cooldown, NOT the hard preconditions. */
  CHECK(capture_decision(Trigger::TEST, off, true, true, true, false,
                         false, 100000, 0, false) == Decision::CAPTURE);
  CHECK(capture_decision(Trigger::TEST, off, true, false, true, false,
                         false, 100000, 0, false) == Decision::SKIP_NO_SD);

  /* Cooldown: alarm cadences re-fire continuously; one capture per window. */
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         false, 100000, 70000, true) == Decision::SKIP_COOLDOWN);
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         false, 130000, 70000, true) == Decision::CAPTURE);
  /* TEST ignores cooldown. */
  CHECK(capture_decision(Trigger::TEST, cfg, true, true, true, false,
                         false, 100000, 70000, true) == Decision::CAPTURE);

  /* millis() wrap: last capture just before wrap, now just after — the
   * unsigned subtraction must yield the true small elapsed time. */
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         false, near_wrap + 30000u /* wraps */, near_wrap,
                         true) == Decision::SKIP_COOLDOWN);
  CHECK(capture_decision(Trigger::T3_SMOKE, cfg, true, true, true, false,
                         false, near_wrap + 70000u /* wraps */, near_wrap,
                         true) == Decision::CAPTURE);

  /* Unknown trigger byte never captures. */
  CHECK(capture_decision(Trigger::NONE, cfg, true, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_BAD_TRIGGER);
  CHECK(capture_decision((Trigger)42, cfg, true, true, true, false,
                         false, 100000, 0, false) == Decision::SKIP_BAD_TRIGGER);
}

static void test_filename() {
  char name[32];
  filename_build(6551, Trigger::T3_SMOKE, name, sizeof(name));
  CHECK(strcmp(name, "seal_00006551_smoke.svlt") == 0);
  filename_build(7, Trigger::TEST, name, sizeof(name));
  CHECK(strcmp(name, "seal_00000007_test.svlt") == 0);
  /* Sequence wraps into 8 digits rather than widening the name. */
  filename_build(123456789, Trigger::T4_CO, name, sizeof(name));
  CHECK(strcmp(name, "seal_23456789_co.svlt") == 0);

  uint32_t seq = 0;
  Trigger t = Trigger::NONE;
  CHECK(filename_parse("seal_00006551_smoke.svlt", &seq, &t));
  CHECK(seq == 6551 && t == Trigger::T3_SMOKE);
  CHECK(filename_parse("seal_00000001_glass.svlt", &seq, &t));
  CHECK(seq == 1 && t == Trigger::GLASS);

  CHECK(!filename_parse("seal_0006551_smoke.svlt", &seq, &t));  /* 7 digits */
  CHECK(!filename_parse("seal_00006551_nope.svlt", &seq, &t));  /* bad tag */
  CHECK(!filename_parse("seal_00006551_smoke.jpg", &seq, &t));  /* bad ext */
  CHECK(!filename_parse("witness_001.jsonl", &seq, &t));
  CHECK(!filename_parse(nullptr, &seq, &t));
}

int main() {
  test_header_golden_and_roundtrip();
  test_header_rejects_malformed();
  test_capture_decision();
  test_filename();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL vault_logic TESTS PASSED\n");
  return 0;
}
