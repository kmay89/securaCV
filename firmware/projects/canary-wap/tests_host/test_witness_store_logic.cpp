/* Host tests for witness_store.h — the /WITNESS/records.jsonl byte-exact
 * line format, tail recovery parse, the SD-wins reconciliation decision,
 * and the seq-binding property: a tail line whose seq was edited while
 * keeping a genuine ch/sig pair must be caught by the chain-hash
 * recompute the device recovery performs. Build & run (CI: firmware.yml
 * host tests; OpenSSL for the recompute check):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_witness_store_logic.cpp \
 *       -lcrypto -o /tmp/test_witness_store_logic && /tmp/test_witness_store_logic
 *
 * GOLDEN_LINE below is the byte-exact output of line_build() for a
 * deterministic record — if the on-card format ever shifts, this fails
 * and the off-device verifier expectations must be revisited with it.
 */

#include <cstdio>
#include <cstring>

#include <openssl/sha.h>

#include "witness_store.h"

using namespace witness_store;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

/* Deterministic fixture: seq=42, tb=7, type=1, payload_hash = 00..1f,
 * prev = 40..5f, chain = 80..9f, sig = a0..df. */
static void fill_fixture(uint8_t ph[32], uint8_t prev[32], uint8_t ch[32],
                         uint8_t sig[64]) {
  for (int i = 0; i < 32; i++) {
    ph[i] = (uint8_t)i;
    prev[i] = (uint8_t)(0x40 + i);
    ch[i] = (uint8_t)(0x80 + i);
  }
  for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(0xA0 + i);
}

static const char GOLDEN_LINE[] =
    "{\"v\":1,\"seq\":42,\"tb\":7,\"type\":1,"
    "\"ph\":\"000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f\","
    "\"prev\":\"404142434445464748494a4b4c4d4e4f"
    "505152535455565758595a5b5c5d5e5f\","
    "\"ch\":\"808182838485868788898a8b8c8d8e8f"
    "909192939495969798999a9b9c9d9e9f\","
    "\"sig\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
    "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf\"}\n";

static void test_line_golden_and_roundtrip() {
  uint8_t ph[32], prev[32], ch[32], sig[64];
  fill_fixture(ph, prev, ch, sig);

  char line[RECORD_LINE_MAX];
  const size_t n = line_build(line, sizeof(line), 42, 7, 1, ph, prev, ch, sig);
  CHECK(n == strlen(GOLDEN_LINE));
  CHECK(strcmp(line, GOLDEN_LINE) == 0);

  /* Parse it back — line_parse wants [line, line_end) of one line. All
   * chain-hash pre-image fields must round-trip, not just head+sig, so
   * the device recovery can bind seq/tb to the signature. */
  TailRecord rec;
  CHECK(line_parse(line, line + n, &rec));
  CHECK(rec.seq == 42);
  CHECK(rec.tb == 7);
  CHECK(memcmp(rec.ph, ph, 32) == 0);
  CHECK(memcmp(rec.prev, prev, 32) == 0);
  CHECK(memcmp(rec.ch, ch, 32) == 0);
  CHECK(memcmp(rec.sig, sig, 64) == 0);

  /* A too-small buffer must refuse, not truncate. */
  char tiny[64];
  CHECK(line_build(tiny, sizeof(tiny), 42, 7, 1, ph, prev, ch, sig) == 0);
}

static void test_tail_parse_picks_newest_complete() {
  uint8_t ph[32], prev[32], ch[32], sig[64];
  fill_fixture(ph, prev, ch, sig);

  char l1[RECORD_LINE_MAX], l2[RECORD_LINE_MAX];
  CHECK(line_build(l1, sizeof(l1), 41, 6, 0, ph, prev, ch, sig) > 0);
  ch[0] = 0xEE; /* distinguish the newer head */
  CHECK(line_build(l2, sizeof(l2), 42, 7, 1, ph, prev, ch, sig) > 0);

  /* Two complete lines + a torn third (power cut mid-append): the torn
   * line has no trailing newline and must be skipped. */
  char tail[3 * RECORD_LINE_MAX];
  snprintf(tail, sizeof(tail), "%s%s{\"v\":1,\"seq\":43,\"tb\":8,\"ty", l1, l2);

  TailRecord rec;
  CHECK(tail_parse(tail, &rec));
  CHECK(rec.seq == 42);
  CHECK(rec.ch[0] == 0xEE);

  /* Only a torn line → nothing recoverable. */
  CHECK(!tail_parse("{\"v\":1,\"seq\":43,\"tb\":8", &rec));
  /* Empty buffer → nothing recoverable. */
  CHECK(!tail_parse("", &rec));

  /* A malformed-but-complete junk line after the good one must not shadow
   * it: the last WELL-FORMED line wins. */
  char tail2[3 * RECORD_LINE_MAX];
  snprintf(tail2, sizeof(tail2), "%sgarbage line without fields\n", l2);
  CHECK(tail_parse(tail2, &rec));
  CHECK(rec.seq == 42);
}

static void test_line_parse_rejects_malformed() {
  TailRecord rec;

  /* Truncated ch hex. */
  const char short_ch[] =
      "{\"v\":1,\"seq\":5,\"ch\":\"abcd\",\"sig\":\"00\"}\n";
  CHECK(!line_parse(short_ch, short_ch + strlen(short_ch) - 1, &rec));

  /* Non-hex characters in ch. */
  char bad_hex[RECORD_LINE_MAX];
  uint8_t ph[32], prev[32], ch[32], fsig[64];
  fill_fixture(ph, prev, ch, fsig);
  const size_t n =
      line_build(bad_hex, sizeof(bad_hex), 9, 1, 0, ph, prev, ch, fsig);
  CHECK(n > 0);
  char* chpos = strstr(bad_hex, "\"ch\":\"");
  CHECK(chpos != NULL);
  chpos[6] = 'z';
  CHECK(!line_parse(bad_hex, bad_hex + n, &rec));

  /* seq overflow (would wrap u32) must be rejected, not wrapped. */
  const char huge_seq[] =
      "{\"v\":1,\"seq\":99999999999,\"ch\":\"000000000000000000000000000000"
      "0000000000000000000000000000000000\",\"sig\":\"0000000000000000000000"
      "000000000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000\"}\n";
  CHECK(!line_parse(huge_seq, huge_seq + strlen(huge_seq) - 1, &rec));

  /* Missing seq entirely. */
  const char no_seq[] = "{\"v\":1,\"ch\":\"00\"}\n";
  CHECK(!line_parse(no_seq, no_seq + strlen(no_seq) - 1, &rec));

  /* u32 boundary: UINT32_MAX itself is a valid seq... */
  char max_line[RECORD_LINE_MAX];
  const size_t mn = line_build(max_line, sizeof(max_line), 4294967295u, 7, 1,
                               ph, prev, ch, fsig);
  CHECK(mn > 0);
  CHECK(line_parse(max_line, max_line + mn, &rec));
  CHECK(rec.seq == 4294967295u);
  /* ...but one past it must be rejected, not wrapped. */
  char over_line[RECORD_LINE_MAX];
  snprintf(over_line, sizeof(over_line), "%s", max_line);
  char* sp = strstr(over_line, "4294967295");
  CHECK(sp != NULL);
  memcpy(sp, "4294967296", 10);
  CHECK(!line_parse(over_line, over_line + mn, &rec));
}

/* The codex-flagged tamper class: the tail's Ed25519 signature covers
 * only the chain hash, so a card editor can change seq while keeping a
 * genuine ch/sig pair. The device recovery must recompute
 * chain_hash(prev, ph, seq, tb) and compare — this test pins that the
 * recompute (mirrored here with OpenSSL over the exact 72-byte layout
 * and domain) matches for the genuine line and mismatches once seq is
 * edited. */
static void host_chain_hash(const uint8_t prev[32], const uint8_t ph[32],
                            uint32_t seq, uint32_t tb, uint8_t out[32]) {
  uint8_t buf[72];
  memcpy(buf, prev, 32);
  memcpy(buf + 32, ph, 32);
  buf[64] = (uint8_t)(seq >> 24); buf[65] = (uint8_t)(seq >> 16);
  buf[66] = (uint8_t)(seq >> 8);  buf[67] = (uint8_t)(seq);
  buf[68] = (uint8_t)(tb >> 24);  buf[69] = (uint8_t)(tb >> 16);
  buf[70] = (uint8_t)(tb >> 8);   buf[71] = (uint8_t)(tb);

  static const char domain[] = "securacv:fw:chain:v1";
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, domain, sizeof(domain) - 1);
  const uint8_t sep = 0x00;
  SHA256_Update(&ctx, &sep, 1);
  SHA256_Update(&ctx, buf, sizeof(buf));
  SHA256_Final(out, &ctx);
}

static void test_seq_tamper_breaks_chain_hash_binding() {
  uint8_t ph[32], prev[32], sig[64];
  for (int i = 0; i < 32; i++) {
    ph[i] = (uint8_t)i;
    prev[i] = (uint8_t)(0x40 + i);
  }
  for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(0xA0 + i);

  /* A REAL chain line: ch is the actual chain hash of (prev, ph, 42, 7). */
  uint8_t ch[32];
  host_chain_hash(prev, ph, 42, 7, ch);
  char line[RECORD_LINE_MAX];
  const size_t n = line_build(line, sizeof(line), 42, 7, 1, ph, prev, ch, sig);
  CHECK(n > 0);

  /* Genuine line: parsed fields recompute to the stored ch. */
  TailRecord rec;
  CHECK(line_parse(line, line + n, &rec));
  uint8_t recomputed[32];
  host_chain_hash(rec.prev, rec.ph, rec.seq, rec.tb, recomputed);
  CHECK(memcmp(recomputed, rec.ch, 32) == 0);

  /* Tampered seq (42 → 99), genuine ch/sig kept: parse still succeeds,
   * but the recompute MUST mismatch — this is what stops a tampered card
   * from moving the device sequence. */
  char* sp = strstr(line, "\"seq\":42");
  CHECK(sp != NULL);
  memcpy(sp + 6, "99", 2); /* digits start after the 6-char "seq": key */
  CHECK(line_parse(line, line + n, &rec));
  CHECK(rec.seq == 99);
  host_chain_hash(rec.prev, rec.ph, rec.seq, rec.tb, recomputed);
  CHECK(memcmp(recomputed, rec.ch, 32) != 0);

  /* Same for a tampered time bucket. */
  host_chain_hash(rec.prev, rec.ph, 42, 8, recomputed);
  CHECK(memcmp(recomputed, rec.ch, 32) != 0);
}

static void test_sd_wins_decision() {
  /* Strictly ahead → adopt. */
  CHECK(sd_wins(5, 6));
  CHECK(sd_wins(0, 1));
  /* Equal → NVS is current, nothing to do. */
  CHECK(!sd_wins(5, 5));
  /* Behind → chain advanced while the card was absent (or an old card
   * came back); adopting would rewind the chain. */
  CHECK(!sd_wins(5, 4));
  CHECK(!sd_wins(1, 0));
}

static void test_hex_helpers() {
  const uint8_t bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  char hex[9];
  to_hex(hex, bytes, 4);
  CHECK(strcmp(hex, "deadbeef") == 0);
  uint8_t back[4] = {0};
  CHECK(from_hex(back, hex, 4));
  CHECK(memcmp(back, bytes, 4) == 0);
  CHECK(!from_hex(back, "deadbeeX", 4));
  CHECK(!from_hex(back, "DEADBEEF", 4)); /* format is lowercase-only */
}

int main() {
  test_hex_helpers();
  test_line_golden_and_roundtrip();
  test_tail_parse_picks_newest_complete();
  test_line_parse_rejects_malformed();
  test_seq_tamper_breaks_chain_hash_binding();
  test_sd_wins_decision();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL witness_store TESTS PASSED\n");
  return 0;
}
