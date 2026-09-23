/*
 * SecuraCV Canary — Opera revocation deny-list — implementation
 *
 * Pure; see mesh_revocation.h. Canonical here, staged byte-identical into
 * the canary-wap sketch (check_mesh_sync.sh).
 */

#include "mesh_revocation.h"

#include <string.h>

namespace mesh_revocation {

namespace {

/* Grace left at `now_ms` (0 = expired). Wrap-safe for spans under 2^32 ms,
 * which expire() running every loop pass guarantees. */
uint32_t left(const Entry& e, uint32_t now_ms) {
  const uint32_t elapsed = now_ms - e.since_ms;
  return elapsed >= e.remaining_ms ? 0 : e.remaining_ms - elapsed;
}

Entry* find(List& list, const uint8_t fp[FP_LEN]) {
  for (size_t i = 0; i < MAX_REVOKED; ++i) {
    if (list.entries[i].in_use && memcmp(list.entries[i].fp, fp, FP_LEN) == 0) {
      return &list.entries[i];
    }
  }
  return nullptr;
}

/* Put (fp, grace) on the list, re-based at now_ms: an existing entry keeps
 * the longer of the two; a new one takes a free slot, else evicts the entry
 * with the least grace left. */
void put(List& list, const uint8_t fp[FP_LEN], uint32_t grace, uint32_t now_ms) {
  Entry* e = find(list, fp);
  if (e != nullptr) {
    const uint32_t have = left(*e, now_ms);
    e->since_ms     = now_ms;
    e->remaining_ms = have > grace ? have : grace;
    return;
  }
  for (size_t i = 0; i < MAX_REVOKED && e == nullptr; ++i) {
    if (!list.entries[i].in_use) e = &list.entries[i];
  }
  if (e == nullptr) {
    e = &list.entries[0];
    for (size_t i = 1; i < MAX_REVOKED; ++i) {
      if (left(list.entries[i], now_ms) < left(*e, now_ms)) e = &list.entries[i];
    }
  }
  memcpy(e->fp, fp, FP_LEN);
  e->since_ms     = now_ms;
  e->remaining_ms = grace;
  e->in_use       = true;
}

}  // namespace

void init(List& list) { memset(&list, 0, sizeof(list)); }

bool add(List& list, const uint8_t fp[FP_LEN], uint32_t now_ms) {
  if (fp == nullptr) return false;
  expire(list, now_ms);
  /* A new removal re-arms the full grace, however much an older entry for
   * the same device had left. */
  Entry* e = find(list, fp);
  if (e != nullptr) {
    e->since_ms     = now_ms;
    e->remaining_ms = REVOCATION_GRACE_MS;
    return true;
  }
  put(list, fp, REVOCATION_GRACE_MS, now_ms);
  return true;
}

bool contains(const List& list, const uint8_t fp[FP_LEN], uint32_t now_ms) {
  if (fp == nullptr) return false;
  for (size_t i = 0; i < MAX_REVOKED; ++i) {
    const Entry& e = list.entries[i];
    if (e.in_use && memcmp(e.fp, fp, FP_LEN) == 0) return left(e, now_ms) > 0;
  }
  return false;
}

size_t expire(List& list, uint32_t now_ms) {
  size_t dropped = 0;
  for (size_t i = 0; i < MAX_REVOKED; ++i) {
    Entry& e = list.entries[i];
    if (e.in_use && left(e, now_ms) == 0) {
      memset(&e, 0, sizeof(e));
      ++dropped;
    }
  }
  return dropped;
}

size_t count(const List& list, uint32_t now_ms) {
  size_t n = 0;
  for (size_t i = 0; i < MAX_REVOKED; ++i) {
    if (list.entries[i].in_use && left(list.entries[i], now_ms) > 0) ++n;
  }
  return n;
}

size_t encode(const List& list, uint32_t now_ms, uint8_t* out, size_t cap) {
  if (out == nullptr) return 0;
  size_t off = 0;
  for (size_t i = 0; i < MAX_REVOKED; ++i) {
    const Entry& e = list.entries[i];
    if (!e.in_use) continue;
    const uint32_t rem = left(e, now_ms);
    if (rem == 0) continue;
    if (off + ENTRY_LEN > cap) return 0;
    memcpy(out + off, e.fp, FP_LEN);
    out[off + FP_LEN + 0] = (uint8_t)(rem);
    out[off + FP_LEN + 1] = (uint8_t)(rem >> 8);
    out[off + FP_LEN + 2] = (uint8_t)(rem >> 16);
    out[off + FP_LEN + 3] = (uint8_t)(rem >> 24);
    off += ENTRY_LEN;
  }
  return off;
}

bool decode(List& list, const uint8_t* blob, size_t len, uint32_t now_ms) {
  if (len % ENTRY_LEN != 0 || len > BLOB_MAX) return false;
  if (len > 0 && blob == nullptr) return false;
  for (size_t off = 0; off < len; off += ENTRY_LEN) {
    uint32_t rem = (uint32_t)blob[off + FP_LEN] |
                   ((uint32_t)blob[off + FP_LEN + 1] << 8) |
                   ((uint32_t)blob[off + FP_LEN + 2] << 16) |
                   ((uint32_t)blob[off + FP_LEN + 3] << 24);
    if (rem == 0) continue;
    /* A stored grace longer than the spec's is a corrupt value, not a
     * longer denial: clamp it. */
    if (rem > REVOCATION_GRACE_MS) rem = REVOCATION_GRACE_MS;
    put(list, blob + off, rem, now_ms);
  }
  return true;
}

}  /* namespace mesh_revocation */
