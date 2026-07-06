/* Host tests for witness_store.h — the /WITNESS/records.jsonl byte-exact
 * line format, tail recovery parse, and the SD-wins reconciliation
 * decision. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_witness_store_logic.cpp \
 *       -o /tmp/test_witness_store_logic && /tmp/test_witness_store_logic
 *
 * GOLDEN_LINE below is the byte-exact output of line_build() for a
 * deterministic record — if the on-card format ever shifts, this fails
 * and the off-device verifier expectations must be revisited with it.
 */

#include <cstdio>
#include <cstring>

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

  /* Parse it back — line_parse wants [line, line_end) of one line. */
  uint32_t seq = 0;
  uint8_t head[32], psig[64];
  CHECK(line_parse(line, line + n, &seq, head, psig));
  CHECK(seq == 42);
  CHECK(memcmp(head, ch, 32) == 0);
  CHECK(memcmp(psig, sig, 64) == 0);

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

  uint32_t seq = 0;
  uint8_t head[32], psig[64];
  CHECK(tail_parse(tail, &seq, head, psig));
  CHECK(seq == 42);
  CHECK(head[0] == 0xEE);

  /* Only a torn line → nothing recoverable. */
  CHECK(!tail_parse("{\"v\":1,\"seq\":43,\"tb\":8", &seq, head, psig));
  /* Empty buffer → nothing recoverable. */
  CHECK(!tail_parse("", &seq, head, psig));

  /* A malformed-but-complete junk line after the good one must not shadow
   * it: the last WELL-FORMED line wins. */
  char tail2[3 * RECORD_LINE_MAX];
  snprintf(tail2, sizeof(tail2), "%sgarbage line without fields\n", l2);
  CHECK(tail_parse(tail2, &seq, head, psig));
  CHECK(seq == 42);
}

static void test_line_parse_rejects_malformed() {
  uint32_t seq = 0;
  uint8_t head[32], sig[64];

  /* Truncated ch hex. */
  const char short_ch[] =
      "{\"v\":1,\"seq\":5,\"ch\":\"abcd\",\"sig\":\"00\"}\n";
  CHECK(!line_parse(short_ch, short_ch + strlen(short_ch) - 1, &seq, head,
                    sig));

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
  CHECK(!line_parse(bad_hex, bad_hex + n, &seq, head, sig));

  /* seq overflow (would wrap u32) must be rejected, not wrapped. */
  const char huge_seq[] =
      "{\"v\":1,\"seq\":99999999999,\"ch\":\"000000000000000000000000000000"
      "0000000000000000000000000000000000\",\"sig\":\"0000000000000000000000"
      "000000000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000\"}\n";
  CHECK(!line_parse(huge_seq, huge_seq + strlen(huge_seq) - 1, &seq, head,
                    sig));

  /* Missing seq entirely. */
  const char no_seq[] = "{\"v\":1,\"ch\":\"00\"}\n";
  CHECK(!line_parse(no_seq, no_seq + strlen(no_seq) - 1, &seq, head, sig));
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
  test_sd_wins_decision();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL witness_store TESTS PASSED\n");
  return 0;
}
