/**
 * @file witness_store.h
 * @brief Pure logic for the /WITNESS append-only SD log: NDJSON line
 *        build/parse and the boot reconciliation decision.
 *
 * The witness chain's durable tier is a single append-only file,
 * /WITNESS/records.jsonl — one self-describing JSON line per signed
 * record, never rotated or truncated (Invariant IV: the sealed log is a
 * locally owned, tamper-evident record; log rotation must never touch
 * it). NVS holds only the chain head + sequence as a fast-boot cache,
 * persisted every SD_PERSIST_INTERVAL records. On boot the tail of the
 * SD log is parsed and, when it is strictly ahead of NVS AND its line
 * signature verifies under this device's public key, SD wins — deriving the
 * head from a stale NVS cache alone would fork the supposedly append-only
 * chain. The beacon audit log (beacon_channel.cpp sd_recover_chain_head)
 * follows the same SD-wins-on-disagreement posture, but its integrity guard
 * is weaker and different: the beacon format carries no per-line sequence and
 * its entries are peer-authored, so it cannot use this chain's seq-ahead or
 * device-key checks. It instead requires the recovered tail to CHAIN-LINK
 * (each entry's prev == the previous entry's head) before adopting a head
 * (beacon_audit_recover.h), with the per-beacon Ed25519 signatures remaining
 * the primary tamper-evidence for entry contents.
 *
 * This header is pure hosted C++ (no Arduino/ESP-IDF includes) so the
 * byte-exact line format and the reconciliation decision are unit-tested
 * on the host (test_witness_store_logic.cpp). The firmware supplies the
 * SD/NVS/crypto glue. The off-device half is tools/verify_witness_log.py,
 * which re-verifies the whole file (chain + Ed25519) from the card alone.
 *
 * Canonical source: firmware/common/witness/witness_store.h. Consumers:
 * the canary-wap sketch (byte-identical staged copy, guarded by
 * check_csi_sync.sh) and the PIO canary tree (firmware/canary, which
 * includes this file directly via -I ../common).
 */

#ifndef WITNESS_STORE_H
#define WITNESS_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace witness_store {

// One line: {"v":1,"seq":N,"tb":N,"type":N,"ph":64x,"prev":64x,"ch":64x,
// "sig":128x}\n — 320 hex chars + framing lands well under 512.
// (Named RECORD_LINE_MAX because POSIX LINE_MAX is a numeric macro on
// newlib/ESP32 — `witness_store::LINE_MAX` would preprocess into
// `witness_store::2048` and fail to compile on-target.)
constexpr size_t RECORD_LINE_MAX = 512;

// A 1 KiB tail always contains at least one complete line plus whatever
// torn final line a power cut left behind.
constexpr size_t TAIL_READ = 1024;

inline void to_hex(char* out, const uint8_t* in, size_t len) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

/* Caller contract: `in` must have at least 2*out_len readable bytes
 * (line_parse bounds every field against line_end before calling; a NUL
 * or any other non-hex byte inside the window fails the parse in-bounds).
 * Lowercase-only by design: line_build emits lowercase, and this is a
 * canonical-format check, not a general hex reader — an uppercase digit
 * means the line is not something this firmware wrote. */
inline bool from_hex(uint8_t* out, const char* in, size_t out_len) {
  for (size_t i = 0; i < out_len; i++) {
    uint8_t v = 0;
    for (int n = 0; n < 2; n++) {
      const char c = in[2 * i + n];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
      else return false;
    }
    out[i] = v;
  }
  return true;
}

/**
 * Build one NDJSON line for a signed witness record. Returns the line
 * length (including the trailing newline), or 0 when the buffer is too
 * small. Field order is fixed; the line is self-describing so an
 * off-device verifier can recompute chain_hash from (prev, ph, seq, tb)
 * and check sig against the device public key — the same fields the BLE
 * witness export emits.
 */
inline size_t line_build(char* buf, size_t buflen, uint32_t seq,
                         uint32_t time_bucket, uint8_t type,
                         const uint8_t payload_hash[32],
                         const uint8_t prev_hash[32],
                         const uint8_t chain_hash[32],
                         const uint8_t signature[64]) {
  char ph[65], prev[65], ch[65], sig[129];
  to_hex(ph, payload_hash, 32);
  to_hex(prev, prev_hash, 32);
  to_hex(ch, chain_hash, 32);
  to_hex(sig, signature, 64);
  const int n = snprintf(buf, buflen,
                         "{\"v\":1,\"seq\":%u,\"tb\":%u,\"type\":%u,"
                         "\"ph\":\"%s\",\"prev\":\"%s\",\"ch\":\"%s\","
                         "\"sig\":\"%s\"}\n",
                         (unsigned)seq, (unsigned)time_bucket,
                         (unsigned)type, ph, prev, ch, sig);
  if (n <= 0 || (size_t)n >= buflen) return 0;
  return (size_t)n;
}

/**
 * All fields the boot reconciliation needs from one parsed line. Every
 * field of the chain-hash pre-image (prev, ph, seq, tb) is extracted, not
 * just the head+sig: the tail's Ed25519 signature covers only the chain
 * hash, so a verifier that never recomputes that hash from the line's own
 * fields would accept a line whose seq (or tb) was edited while keeping a
 * genuine ch/sig pair — letting a tampered card move the device sequence
 * arbitrarily. The device glue must recompute
 * chain_hash(prev, ph, seq, tb) and require it to equal ch before
 * adopting, exactly as tools/verify_witness_log.py does offline.
 */
struct TailRecord {
  uint32_t seq;
  uint32_t tb;
  uint8_t  ph[32];
  uint8_t  prev[32];
  uint8_t  ch[32];
  uint8_t  sig[64];
};

/* Parse an unsigned decimal following `key` (e.g. "\"seq\":") within
 * [line, line_end). Rejects overflow past UINT32_MAX rather than
 * wrapping into a bogus-but-valid value. */
inline bool parse_u32_field(const char* line, const char* line_end,
                            const char* key, size_t key_len,
                            uint32_t* out) {
  const char* s = strstr(line, key);
  if (s == NULL || s >= line_end) return false;
  s += key_len;
  uint32_t v = 0;
  bool any = false;
  while (s < line_end && *s >= '0' && *s <= '9') {
    const uint32_t d = (uint32_t)(*s - '0');
    // UINT32_MAX is 4294967295: one more digit overflows when the
    // accumulated value exceeds 429496729, or equals it with d > 5.
    if (v > 429496729u || (v == 429496729u && d > 5)) return false;
    v = v * 10u + d;
    s++;
    any = true;
  }
  if (!any) return false;
  *out = v;
  return true;
}

/* Parse a fixed-width hex string field `key` (e.g. "\"ch\":\"") within
 * [line, line_end) into out (n_bytes decoded bytes). */
inline bool parse_hex_field(const char* line, const char* line_end,
                            const char* key, size_t key_len,
                            uint8_t* out, size_t n_bytes) {
  const char* s = strstr(line, key);
  if (s == NULL || s + key_len + 2 * n_bytes > line_end) return false;
  return from_hex(out, s + key_len, n_bytes);
}

/**
 * Parse one complete line (must span [line, line_end)) into a TailRecord.
 * Returns false on any malformed field. Tolerant of unknown extra keys
 * but every extracted field must be well-formed — the full chain-hash
 * pre-image is required so the caller can bind seq/tb to the signature
 * (see TailRecord).
 */
inline bool line_parse(const char* line, const char* line_end,
                       TailRecord* out) {
  if (!parse_u32_field(line, line_end, "\"seq\":", 6, &out->seq)) return false;
  if (!parse_u32_field(line, line_end, "\"tb\":", 5, &out->tb)) return false;
  if (!parse_hex_field(line, line_end, "\"ph\":\"", 6, out->ph, 32)) return false;
  if (!parse_hex_field(line, line_end, "\"prev\":\"", 8, out->prev, 32)) return false;
  if (!parse_hex_field(line, line_end, "\"ch\":\"", 6, out->ch, 32)) return false;
  if (!parse_hex_field(line, line_end, "\"sig\":\"", 7, out->sig, 64)) return false;
  return true;
}

/**
 * Parse the newest complete line out of a NUL-terminated tail buffer.
 * Complete = newline-terminated: a torn final line (power cut mid-append)
 * has no trailing '\n' and is skipped, falling back to the previous
 * line — exactly the head that torn line chained from. Returns false when
 * no complete, well-formed line exists in the tail.
 */
inline bool tail_parse(const char* tail, TailRecord* out) {
  bool found = false;
  const char* p = tail;
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    if (nl == NULL) break;  // torn final line — stop at the last complete one
    TailRecord rec;
    if (line_parse(p, nl, &rec)) {
      *out = rec;
      found = true;
    }
    p = nl + 1;
  }
  return found;
}

/**
 * Boot reconciliation decision: adopt the SD tail only when it is
 * STRICTLY ahead of the NVS cache. Equal → nothing to do; behind → the
 * chain advanced in RAM/NVS while the card was absent (or an old card
 * was re-inserted) and adopting would rewind the chain.
 */
inline bool sd_wins(uint32_t nvs_seq, uint32_t sd_seq) {
  return sd_seq > nvs_seq;
}

}  // namespace witness_store

#endif  // WITNESS_STORE_H
