/* Host tests for the atomic chain-state blob (firmware/common/witness/chain_state.h).
 *
 * The blob replaces a two-entry NVS write (seq, then chain head) that a power
 * cut could tear. Every test here is about a way a torn, foreign or damaged
 * entry could be mistaken for a chain state, or a way the boot-time source
 * decision could pick the wrong truth — so the suite is organized by the
 * failure prevented, not by the function called.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common test_chain_state.cpp
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "witness/chain_state.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using chain_state::BLOB_LEN;
using chain_state::HEAD_LEN;
using chain_state::Source;

static void fill_head(uint8_t head[HEAD_LEN]) {
  for (size_t i = 0; i < HEAD_LEN; ++i) head[i] = (uint8_t)i;   // 0x00..0x1f
}

// ── the format is exactly what the header says: pinned bytes ────────────────
//
// seq = 1, head = 0x00..0x1f. Version first, seq little-endian, the head
// verbatim, CRC-16/CCITT-FALSE big-endian. A change to any of that is a wire
// change and must move this table on purpose.
static void test_golden_bytes() {
  static const uint8_t kGolden[BLOB_LEN] = {
    0x01,                                               // version
    0x01, 0x00, 0x00, 0x00,                             // seq = 1, LE
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,     // head
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0xbb, 0x5c                                          // CRC-16/CCITT-FALSE
  };
  uint8_t head[HEAD_LEN]; fill_head(head);
  uint8_t out[BLOB_LEN]; std::memset(out, 0xEE, sizeof out);
  CHECK(chain_state::encode(1, head, out));
  CHECK(std::memcmp(out, kGolden, BLOB_LEN) == 0);
  CHECK(BLOB_LEN == 39);
  CHECK(chain_state::VERSION == 0x01);
  // The CRC is the published CRC-16/CCITT-FALSE (check value for "123456789").
  CHECK(chain_state::crc16_ccitt((const uint8_t*)"123456789", 9) == 0x29B1);
}

// ── what goes in comes out, at the edges too ────────────────────────────────
static void test_round_trip() {
  const uint32_t seqs[] = { 0u, 1u, 255u, 256u, 65535u, 65536u, 0x12345678u, 0xFFFFFFFFu };
  for (uint32_t seq : seqs) {
    uint8_t head[HEAD_LEN]; fill_head(head);
    head[0] = (uint8_t)(seq & 0xFF);            // vary the head with the seq
    head[HEAD_LEN - 1] = (uint8_t)(seq >> 24);
    uint8_t blob[BLOB_LEN];
    CHECK(chain_state::encode(seq, head, blob));
    uint32_t got_seq = 0xDEADBEEFu; uint8_t got_head[HEAD_LEN]; std::memset(got_head, 0xAA, HEAD_LEN);
    CHECK(chain_state::decode(blob, BLOB_LEN, &got_seq, got_head));
    CHECK(got_seq == seq);
    CHECK(std::memcmp(got_head, head, HEAD_LEN) == 0);
  }
}

// ── the failure: a torn or foreign entry read as a chain state ──────────────
//
// A wrong length (a half-written blob, or a 32-byte legacy `chain` value that
// somehow landed under the new key) is "no blob", never a guess.
static void test_wrong_length_rejected() {
  uint8_t head[HEAD_LEN]; fill_head(head);
  uint8_t blob[BLOB_LEN + 1];
  CHECK(chain_state::encode(7, head, blob));
  uint32_t seq = 0; uint8_t h[HEAD_LEN];
  CHECK(!chain_state::decode(blob, BLOB_LEN - 1, &seq, h));   // 38: truncated
  CHECK(!chain_state::decode(blob, BLOB_LEN + 1, &seq, h));   // 40: overlong
  CHECK(!chain_state::decode(blob, HEAD_LEN, &seq, h));       // 32: a bare head
  CHECK(!chain_state::decode(blob, 0, &seq, h));
  CHECK(chain_state::decode(blob, BLOB_LEN, &seq, h));        // the exact length still works
  CHECK(seq == 7);
}

// ── the failure: a future format decoded by an old reader ───────────────────
static void test_wrong_version_rejected() {
  uint8_t head[HEAD_LEN]; fill_head(head);
  uint8_t blob[BLOB_LEN];
  CHECK(chain_state::encode(9, head, blob));
  blob[0] = 0x02;
  // Re-seal the CRC so the version byte is the ONLY thing wrong.
  const uint16_t crc = chain_state::crc16_ccitt(blob, chain_state::CHECKED_LEN);
  blob[chain_state::CHECKED_LEN] = (uint8_t)(crc >> 8);
  blob[chain_state::CHECKED_LEN + 1] = (uint8_t)(crc & 0xFF);
  uint32_t seq = 0; uint8_t h[HEAD_LEN];
  CHECK(!chain_state::decode(blob, BLOB_LEN, &seq, h));
  blob[0] = 0x00;
  CHECK(!chain_state::decode(blob, BLOB_LEN, &seq, h));
}

// ── the failure: one damaged bit adopted as the chain head ──────────────────
//
// Every single bit of the 37 checked bytes, flipped one at a time, is caught —
// and so is every bit of the CRC itself.
static void test_single_bit_flip_rejected() {
  uint8_t head[HEAD_LEN]; fill_head(head);
  uint8_t good[BLOB_LEN];
  CHECK(chain_state::encode(0x01020304u, head, good));
  for (size_t byte = 0; byte < BLOB_LEN; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      uint8_t blob[BLOB_LEN]; std::memcpy(blob, good, BLOB_LEN);
      blob[byte] ^= (uint8_t)(1u << bit);
      uint32_t seq = 0; uint8_t h[HEAD_LEN];
      CHECK(!chain_state::decode(blob, BLOB_LEN, &seq, h));
    }
  }
}

// ── the failure Fletcher-16 would NOT have caught: 0x00 <-> 0xFF ────────────
//
// Erased flash reads 0xFF. A checksum that cannot tell a 0x00 byte from a
// 0xFF byte (Fletcher's mod-255 sums) would accept seq = 0 for seq =
// 0xFFFFFFFF and a zero head byte for an erased one. Pin that ours does not.
static void test_zero_ff_substitution_rejected() {
  uint8_t head[HEAD_LEN]; std::memset(head, 0, HEAD_LEN);
  uint8_t blob0[BLOB_LEN], blobF[BLOB_LEN];
  CHECK(chain_state::encode(0u, head, blob0));
  CHECK(chain_state::encode(0xFFFFFFFFu, head, blobF));
  CHECK(std::memcmp(blob0 + chain_state::CHECKED_LEN, blobF + chain_state::CHECKED_LEN, 2) != 0);
  // Swap every seq byte 0x00 -> 0xFF under the seq-0 CRC: must be rejected.
  uint8_t forged[BLOB_LEN]; std::memcpy(forged, blob0, BLOB_LEN);
  for (size_t i = 1; i <= 4; ++i) forged[i] = 0xFF;
  uint32_t seq = 0; uint8_t h[HEAD_LEN];
  CHECK(!chain_state::decode(forged, BLOB_LEN, &seq, h));
  // And one head byte erased to 0xFF under the all-zero-head CRC: rejected.
  std::memcpy(forged, blob0, BLOB_LEN);
  forged[5 + 17] = 0xFF;
  CHECK(!chain_state::decode(forged, BLOB_LEN, &seq, h));
}

// ── a null argument is a refusal, never a crash and never a write ───────────
static void test_null_args_safe() {
  uint8_t head[HEAD_LEN]; fill_head(head);
  uint8_t blob[BLOB_LEN];
  CHECK(!chain_state::encode(1, nullptr, blob));
  CHECK(!chain_state::encode(1, head, nullptr));
  CHECK(chain_state::encode(1, head, blob));
  uint32_t seq = 77; uint8_t h[HEAD_LEN]; std::memset(h, 0xAA, HEAD_LEN);
  CHECK(!chain_state::decode(nullptr, BLOB_LEN, &seq, h));
  CHECK(!chain_state::decode(blob, BLOB_LEN, nullptr, h));
  CHECK(!chain_state::decode(blob, BLOB_LEN, &seq, nullptr));
  CHECK(seq == 77);                                  // untouched on refusal
  CHECK(h[0] == 0xAA && h[HEAD_LEN - 1] == 0xAA);
  // A rejected (damaged) blob also leaves the outputs alone.
  blob[10] ^= 0x01;
  CHECK(!chain_state::decode(blob, BLOB_LEN, &seq, h));
  CHECK(seq == 77);
  CHECK(h[0] == 0xAA);
}

// ── the source decision: blob beats legacy, legacy beats genesis ────────────
//
// The failure: a fresh image on an upgraded device starting a NEW chain
// (genesis) while a legacy pair sat right there, or trusting a stale legacy
// pair over the blob the last boot wrote.
static void test_choose_table() {
  // The stale-legacy case this image creates itself: legacy frozen at or
  // below the blob's seq -> the blob.
  CHECK(chain_state::choose(true,  true,  100, 50)  == Source::Blob);
  CHECK(chain_state::choose(true,  true,  100, 100) == Source::Blob);  // equal: nothing re-signed either way
  CHECK(chain_state::choose(true,  false, 100, 0)   == Source::Blob);
  CHECK(chain_state::choose(false, true,  0,   50)  == Source::Legacy);  // blob invalid + legacy -> Legacy
  CHECK(chain_state::choose(false, false, 0,   0)   == Source::Genesis);
  // Seqs never matter to the one-sided cases.
  CHECK(chain_state::choose(false, true,  999, 1)   == Source::Legacy);
  CHECK(chain_state::choose(true,  false, 1,   999) == Source::Blob);
  CHECK(chain_state::choose(false, false, 5,   9)   == Source::Genesis);
}

// ── the failure: a re-upgrade re-signing seqs an older image already signed ─
//
// Image N persists the blob at seq 100 (the legacy pair frozen at 50). A
// downgrade to N-1 continues from 50 — its code, its fork — and writes ONLY
// the legacy pair, up to seq 150. Re-upgrading to N with a blob-first rule
// would resume at 100 and sign 101..150 a second time on another branch.
// A legacy seq AHEAD of the blob's means exactly that happened: resume from
// the legacy pair, where the newest records are.
static void test_choose_prefers_newer_legacy_after_reupgrade() {
  CHECK(chain_state::choose(true, true, 100, 150) == Source::Legacy);
  CHECK(chain_state::choose(true, true, 100, 101) == Source::Legacy);   // one ahead is enough
  CHECK(chain_state::choose(true, true, 0,   1)   == Source::Legacy);
  CHECK(chain_state::choose(true, true, 0xFFFFFFFEu, 0xFFFFFFFFu) == Source::Legacy);
  // ...and once N persists again (blob past the frozen pair), the blob wins
  // for good: the decision converges, it does not flap.
  CHECK(chain_state::choose(true, true, 151, 150) == Source::Blob);
}

int main() {
  test_golden_bytes();
  test_round_trip();
  test_wrong_length_rejected();
  test_wrong_version_rejected();
  test_single_bit_flip_rejected();
  test_zero_ff_substitution_rejected();
  test_null_args_safe();
  test_choose_table();
  test_choose_prefers_newer_legacy_after_reupgrade();

  if (g_failures == 0) { std::printf("ALL chain-state tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
