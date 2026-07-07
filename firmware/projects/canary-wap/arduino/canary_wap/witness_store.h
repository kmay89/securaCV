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
 * signature verifies under this device's public key, SD wins — the same
 * two-tier reconciliation the beacon audit log uses (beacon_channel.cpp
 * sd_recover_chain_head): deriving the head from a stale NVS cache alone
 * would fork the supposedly append-only chain.
 *
 * This header is pure hosted C++ (no Arduino/ESP-IDF includes) so the
 * byte-exact line format and the reconciliation decision are unit-tested
 * on the host (test_witness_store_logic.cpp). The sketch supplies the
 * SD/NVS/crypto glue. The off-device half is tools/verify_witness_log.py,
 * which re-verifies the whole file (chain + Ed25519) from the card alone.
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
 * Parse one complete line (must span [line, line_end)) into the fields
 * the boot reconciliation needs: seq, chain hash, signature. Returns
 * false on any malformed field. Tolerant of unknown extra keys but the
 * three extracted fields must be well-formed.
 */
inline bool line_parse(const char* line, const char* line_end,
                       uint32_t* out_seq, uint8_t out_head[32],
                       uint8_t out_sig[64]) {
  const char* s = strstr(line, "\"seq\":");
  if (s == NULL || s >= line_end) return false;
  s += 6;
  uint32_t seq = 0;
  bool any = false;
  while (s < line_end && *s >= '0' && *s <= '9') {
    // Reject overflow rather than wrapping into a bogus-but-valid seq.
    if (seq > 429496728u) return false;
    seq = seq * 10u + (uint32_t)(*s - '0');
    s++;
    any = true;
  }
  if (!any) return false;

  const char* ch = strstr(line, "\"ch\":\"");
  if (ch == NULL || ch + 6 + 64 > line_end) return false;
  if (!from_hex(out_head, ch + 6, 32)) return false;

  const char* sig = strstr(line, "\"sig\":\"");
  if (sig == NULL || sig + 7 + 128 > line_end) return false;
  if (!from_hex(out_sig, sig + 7, 64)) return false;

  *out_seq = seq;
  return true;
}

/**
 * Parse the newest complete line out of a NUL-terminated tail buffer.
 * Complete = newline-terminated: a torn final line (power cut mid-append)
 * has no trailing '\n' and is skipped, falling back to the previous
 * line — exactly the head that torn line chained from. Returns false when
 * no complete, well-formed line exists in the tail.
 */
inline bool tail_parse(const char* tail, uint32_t* out_seq,
                       uint8_t out_head[32], uint8_t out_sig[64]) {
  bool found = false;
  const char* p = tail;
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    if (nl == NULL) break;  // torn final line — stop at the last complete one
    uint32_t seq;
    uint8_t head[32], sig[64];
    if (line_parse(p, nl, &seq, head, sig)) {
      *out_seq = seq;
      memcpy(out_head, head, 32);
      memcpy(out_sig, sig, 64);
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
