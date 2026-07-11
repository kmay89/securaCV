/*
 * SecuraCV Canary WAP — pure beacon-audit SD recovery decision (host-testable)
 *
 * Arduino-free: stdint/string only. The blocking SD read lives in
 * beacon_channel.cpp (sd_recover_chain_head); the branchy DECISION of
 * WHICH head to trust from the file tail lives here so a host g++ run
 * (test_beacon_audit_recover.cpp) can pin it.
 *
 * Why this exists: /beacon/audit.jsonl is the append-only log of record for
 * the mesh beacon audit chain. NVS holds only the chain head as a fast-boot
 * cache; on boot the SD tail is consulted so a stale/wiped NVS cache cannot
 * fork the chain. The recovery MUST NOT adopt a head verbatim from whatever
 * the last "head":"…" substring happens to be — a torn power-cut tail, a
 * corrupt line, or a spliced fragment could then silently redirect the chain.
 *
 * The beacon audit format has no per-line sequence number and its entries are
 * authored by mesh PEERS (each carries its own Ed25519 originator/cosigner
 * signature — the primary tamper-evidence), so the two guards the witness
 * recovery uses (strictly-ahead-of-NVS by seq, and device-key line-signature
 * verify) do not port. The guard that DOES port, using only fields already on
 * disk, is CHAIN LINKAGE: every healthy entry's `prev` equals the previous
 * entry's `head` (head = SHA-256(prev || entry)). So we adopt the newest
 * COMPLETE line's head only when it chain-links to the line before it — or,
 * for a genuine first record, when its `prev` is the all-zero genesis head.
 * A tail that does not link (torn, corrupt, single unverifiable fragment) is
 * refused and the NVS head is kept; adopting an unverifiable head would be
 * exactly the blind trust this guard exists to remove.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BEACON_AUDIT_RECOVER_H
#define SECURACV_BEACON_AUDIT_RECOVER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace beacon_audit_recover {

// Parse 32 bytes of lowercase hex at `in` (must have >= 64 readable chars).
inline bool parse_hex32(const char* in, uint8_t out[32]) {
  for (size_t i = 0; i < 32; i++) {
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

// Find `key` within [line, line+line_len), then parse the 64 hex chars that
// follow it into out[32]. Bounded to the line so a field can never be read
// out of an adjacent line. `key` is e.g. "\"head\":\"".
inline bool field_hex32(const char* line, size_t line_len, const char* key,
                        uint8_t out[32]) {
  const size_t key_len = strlen(key);
  if (line_len < key_len) return false;
  for (size_t i = 0; i + key_len <= line_len; i++) {
    if (memcmp(line + i, key, key_len) == 0) {
      const size_t val_off = i + key_len;
      if (line_len - val_off < 64) return false;
      return parse_hex32(line + val_off, out);
    }
  }
  return false;
}

// Recover the beacon-audit chain head from a tail of /beacon/audit.jsonl.
//
//   tail            : the last `len` bytes of the file (NUL-termination not
//                     required; `len` bounds all reads).
//   len             : number of valid bytes in `tail`.
//   tail_from_start : true iff `tail` begins at file offset 0 (so the first
//                     segment is a complete line, not a mid-line fragment).
//   out_head        : receives the adopted 32-byte head on success.
//
// Returns true (and fills out_head) ONLY when the newest complete line's head
// chain-links to the file: either its `prev` matches the immediately preceding
// complete line's `head`, or — when it is the first record — its `prev` is the
// all-zero genesis. Returns false (adopt nothing; keep the NVS head) for an
// empty/unterminated/torn/corrupt/unlinkable tail.
inline bool recover_head(const char* tail, size_t len, bool tail_from_start,
                         uint8_t out_head[32]) {
  if (tail == nullptr || len == 0) return false;

  // The newest COMPLETE line ends at the last '\n'; anything after it is a
  // torn/in-flight partial write and is ignored.
  size_t nl_last = len;  // index of last '\n', or len if none
  for (size_t i = len; i > 0; i--) {
    if (tail[i - 1] == '\n') { nl_last = i - 1; break; }
  }
  if (nl_last == len) return false;  // no terminated line in the window

  // The newest complete line spans [s1, nl_last). Its start is just after the
  // previous '\n', or offset 0 only when the window covers the file start.
  size_t s1 = 0;
  bool have_s1 = tail_from_start;  // start-of-buffer is a valid line start only from file start
  for (size_t i = nl_last; i > 0; i--) {
    if (tail[i - 1] == '\n') { s1 = i; have_s1 = true; break; }
  }
  if (!have_s1) return false;  // buffer began mid-line; newest line is a fragment
  const char* line1 = tail + s1;
  const size_t line1_len = nl_last - s1;

  uint8_t newest_prev[32];
  uint8_t newest_head[32];
  if (!field_hex32(line1, line1_len, "\"prev\":\"", newest_prev)) return false;
  if (!field_hex32(line1, line1_len, "\"head\":\"", newest_head)) return false;

  // Is there a preceding complete line inside the window? Its end is the '\n'
  // at s1-1; its start is the '\n' before that (or offset 0 from file start).
  bool have_pred = false;
  size_t s0 = 0;
  size_t e0 = 0;
  if (s1 >= 1) {
    e0 = s1 - 1;  // index of the '\n' terminating the predecessor
    bool have_s0 = tail_from_start;
    for (size_t i = e0; i > 0; i--) {
      if (tail[i - 1] == '\n') { s0 = i; have_s0 = true; break; }
    }
    if (have_s0 && e0 > s0) have_pred = true;
  }

  if (have_pred) {
    uint8_t pred_head[32];
    if (!field_hex32(tail + s0, e0 - s0, "\"head\":\"", pred_head)) return false;
    // Chain linkage: the newest entry must chain from the previous one.
    if (memcmp(newest_prev, pred_head, 32) != 0) return false;
  } else {
    // No predecessor in view. Only trust the newest line if it is a genuine
    // first record (prev == all-zero genesis); otherwise its linkage cannot
    // be verified and we refuse (keep the NVS head, fail closed on integrity).
    uint8_t genesis[32];
    memset(genesis, 0, sizeof(genesis));
    if (memcmp(newest_prev, genesis, 32) != 0) return false;
  }

  memcpy(out_head, newest_head, 32);
  return true;
}

}  // namespace beacon_audit_recover

#endif  // SECURACV_BEACON_AUDIT_RECOVER_H
