/**
 * @file chain_state.h
 * @brief The chain head and its sequence as ONE NVS entry — codec and the
 *        boot-time source decision.
 *
 * THE PROBLEM THIS SOLVES. The witness chain's fast-boot cache in NVS was two
 * entries, `seq` (u32) then `chain` (32-byte head), written one after the
 * other. A power cut between the two writes left a seq that belonged to a
 * different head — a torn pair the next boot adopted as truth. The SD log's
 * tail reconciles it (witness_store.h `sd_wins`) but only when a card is
 * present; a card-less Canary had no recovery path (roadmap §3.7, item 18).
 *
 * ESP-IDF NVS commits one entry atomically: a blob write is either fully
 * visible or not at all. So the fix is a format, not a lock — encode {seq,
 * head} as a single 39-byte blob and the two-write window is gone.
 *
 * BLOB LAYOUT (BLOB_LEN = 39 bytes, little-endian seq, CRC big-endian):
 *
 *     [0]      version, 0x01
 *     [1..4]   seq, uint32 little-endian
 *     [5..36]  chain head, 32 bytes
 *     [37..38] CRC-16/CCITT-FALSE over bytes [0..36], big-endian
 *
 * WHY A CRC AND NOT FLETCHER-16. NVS carries its own CRC32 per entry, so this
 * checksum is not the primary integrity guard; it exists so a wrong-format
 * or half-written entry under the same key decodes as "no blob" rather than
 * as a chain state. Fletcher-16 was the first choice and is the wrong one
 * for anything that lives in flash: its mod-255 sums cannot tell a 0x00 byte
 * from a 0xFF byte (seq = 0 and seq = 0xFFFFFFFF sum identically), and
 * erased flash reads 0xFF. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF,
 * check value 0x29B1 for "123456789") detects every single-byte substitution
 * and needs no table.
 *
 * THE SOURCE DECISION. On boot the firmware reads the blob, then the legacy
 * `seq` + `chain` pair, and `choose()` says which to trust: a valid blob wins,
 * else the legacy pair (READ-ONLY — it is never rewritten or deleted, so an
 * older image still boots after a downgrade), else genesis. The legacy pair
 * therefore goes stale after the first blob write; an older image after a
 * downgrade sees it and continues the chain from that stale point — a fork
 * this image cannot prevent (it is the older image's code running), and
 * SD-wins covers it only when a card is present.
 *
 * THE RE-UPGRADE. That older image writes only the legacy pair. When this
 * image comes back, a blob-first rule would resume from the blob — the state
 * from BEFORE the downgrade — and re-sign every seq from blob_seq + 1 to the
 * older image's legacy seq on yet another branch. So `choose()` compares the
 * two seqs when both are present: while this image runs, the blob's seq never
 * falls below the frozen legacy seq (the first blob write starts at or above
 * it, seq only ever increments, and SD-wins only ever raises it), so a legacy
 * seq AHEAD of the blob's can only mean an older image ran since the last
 * blob write, and its pair holds the newest records: resume from it. (Its
 * pair is whatever that image last wrote, torn-write window included — the
 * pre-blob risk, back only for the image that still has it.) Equal seqs keep
 * the blob: nothing is re-signed either way. The SD-wins reconciliation is
 * unchanged and still runs after this.
 *
 * Pure hosted C++ (no Arduino/ESP-IDF includes) so the byte-exact format and
 * the decision are unit-tested on the host (test_chain_state.cpp). The
 * firmware supplies the NVS glue. Canonical source:
 * firmware/common/witness/chain_state.h; consumer: the PIO canary tree
 * (securacv_witness.cpp, via -I ../common).
 */

#ifndef WITNESS_CHAIN_STATE_H
#define WITNESS_CHAIN_STATE_H

#include <stddef.h>
#include <stdint.h>

namespace chain_state {

constexpr uint8_t VERSION = 0x01;
constexpr size_t HEAD_LEN = 32;
constexpr size_t BLOB_LEN = 1 + 4 + HEAD_LEN + 2;   // 39
constexpr size_t CHECKED_LEN = BLOB_LEN - 2;        // 37: everything but the CRC

/** CRC-16/CCITT-FALSE. Bitwise, tableless; 37 bytes is nothing. */
inline uint16_t crc16_ccitt(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc = (uint16_t)(crc ^ ((uint16_t)p[i] << 8));
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/** Pack {seq, head} into `out[BLOB_LEN]`. False only on a null argument. */
inline bool encode(uint32_t seq, const uint8_t head[HEAD_LEN], uint8_t out[BLOB_LEN]) {
  if (!head || !out) return false;
  out[0] = VERSION;
  out[1] = (uint8_t)(seq & 0xFFu);
  out[2] = (uint8_t)((seq >> 8) & 0xFFu);
  out[3] = (uint8_t)((seq >> 16) & 0xFFu);
  out[4] = (uint8_t)((seq >> 24) & 0xFFu);
  for (size_t i = 0; i < HEAD_LEN; ++i) out[5 + i] = head[i];
  const uint16_t crc = crc16_ccitt(out, CHECKED_LEN);
  out[CHECKED_LEN] = (uint8_t)(crc >> 8);
  out[CHECKED_LEN + 1] = (uint8_t)(crc & 0xFFu);
  return true;
}

/**
 * Unpack a blob. False — and nothing written — on a null argument, a length
 * other than BLOB_LEN, an unknown version, or a CRC mismatch. A false here
 * means "there is no blob", never "here is a guess".
 */
inline bool decode(const uint8_t* in, size_t len, uint32_t* seq, uint8_t head[HEAD_LEN]) {
  if (!in || !seq || !head) return false;
  if (len != BLOB_LEN) return false;
  if (in[0] != VERSION) return false;
  const uint16_t want = (uint16_t)(((uint16_t)in[CHECKED_LEN] << 8) | in[CHECKED_LEN + 1]);
  if (crc16_ccitt(in, CHECKED_LEN) != want) return false;
  *seq = (uint32_t)in[1] | ((uint32_t)in[2] << 8) | ((uint32_t)in[3] << 16) |
         ((uint32_t)in[4] << 24);
  for (size_t i = 0; i < HEAD_LEN; ++i) head[i] = in[5 + i];
  return true;
}

/** Where this boot's chain state comes from. */
enum class Source : uint8_t {
  Blob,     ///< the atomic blob decoded — the normal case after the first persist
  Legacy,   ///< no valid blob, or an older image wrote a newer pair (read-only either way)
  Genesis,  ///< neither — a fresh device (or a wiped NVS): start the chain
};

/**
 * The whole decision: blob beats legacy, legacy beats genesis — except that a
 * legacy seq strictly ahead of a valid blob's means an older image ran since
 * the last blob write (see THE RE-UPGRADE above), and then the legacy pair
 * wins. `blob_seq` is read only when `blob_ok`, `legacy_seq` only when
 * `legacy_present` (pass 0 when the legacy seq entry is absent).
 */
inline Source choose(bool blob_ok, bool legacy_present,
                     uint32_t blob_seq, uint32_t legacy_seq) {
  if (blob_ok && legacy_present && legacy_seq > blob_seq) return Source::Legacy;
  if (blob_ok) return Source::Blob;
  if (legacy_present) return Source::Legacy;
  return Source::Genesis;
}

}  // namespace chain_state

#endif  // WITNESS_CHAIN_STATE_H
