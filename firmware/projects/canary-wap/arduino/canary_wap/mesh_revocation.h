/*
 * SecuraCV Canary — Opera revocation deny-list (spec §5.6 REVOCATION_GRACE_MS)
 * Version 0.1.0
 *
 * CRYPTO-ADJACENT (F33 part 6): maintainer review; bench-gated (U1 Track C3).
 *
 * Spec §5.6: "The removed device's pubkey is recorded in a local revocation
 * list and refused acceptance into future pairing flows for
 * REVOCATION_GRACE_MS (default 7 days), even by a freshly-rotated opera."
 * Until F33 neither tree had it: a removed device could be walked straight
 * back in through pairing.
 *
 * This module is the list and nothing else — pure, no I/O, no globals —
 * shared by BOTH mesh trees: this file is canonical, and
 * firmware/projects/canary-wap/arduino/canary_wap/ carries byte-identical
 * copies (firmware/scripts/check_mesh_sync.sh fails on drift). Each tree
 * decides where it records and where it refuses:
 *   • PlatformIO (mesh_session): records the peer this device removes AND
 *     the removed_fp of every verified REKEY_OFFER it hears — even one it
 *     cannot take part in because a rotation of its own is running — and
 *     forgets that peer at once; refuses a deny-listed device as a pairing
 *     partner (its DISCOVER / OFFER is dropped) and as a trusted peer.
 *   • canary-wap (mesh_network.cpp): records the peer this device removes;
 *     refuses it in the pairing handlers and in add_peer(). (Its rotation
 *     message does not name the removed device, so it cannot propagate.)
 *
 * Keyed by the 8-byte fingerprint (SHA-256 of the pubkey, truncated), the
 * identity both trees' pairing and removal already use.
 *
 * Time is uptime (millis()), like every timeout in the mesh. An entry holds
 * its remaining grace and the uptime it was last re-based at, so:
 *   • expire(now) must run at least once per ~49 days of uptime (the u32
 *     wrap) — both trees call it every loop pass;
 *   • across a reboot the list is persisted as (fingerprint, remaining ms)
 *     and restored with that remaining time from the new boot's clock.
 *     Time powered off does not count down: a device that is off for a day
 *     denies for a day longer. That is the conservative direction for a
 *     deny-list; it can never shorten the grace.
 *
 * Bounded at MAX_REVOKED entries; adding to a full list evicts the entry
 * with the least grace left (a removal the user just made always fits).
 *
 * Host test: firmware/canary/lib/securacv_mesh/test_mesh_revocation.cpp (and
 * the canary-wap suite compiles the staged copy).
 */

#ifndef SECURACV_MESH_REVOCATION_H
#define SECURACV_MESH_REVOCATION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_revocation {

constexpr size_t   FP_LEN              = 8;
constexpr size_t   MAX_REVOKED         = 8;
/* Spec §5.6 default: 7 days. 604,800,000 ms fits a u32 (max ~49.7 days). */
constexpr uint32_t REVOCATION_GRACE_MS = 7u * 24u * 60u * 60u * 1000u;

/* NVS blob: MAX_REVOKED entries of fingerprint (8 B) || remaining ms
 * (4 B, little-endian). */
constexpr size_t ENTRY_LEN = FP_LEN + 4;                    /* 12 */
constexpr size_t BLOB_MAX  = MAX_REVOKED * ENTRY_LEN;       /* 96 */

struct Entry {
  uint8_t  fp[FP_LEN];
  uint32_t since_ms;       /* uptime the remaining time counts from */
  uint32_t remaining_ms;   /* grace left at since_ms */
  bool     in_use;
};

struct List {
  Entry entries[MAX_REVOKED];
};

void init(List& list);

/* Record (or re-arm, with the full grace) a revoked fingerprint. Returns
 * false only on a null fingerprint. */
bool add(List& list, const uint8_t fp[FP_LEN], uint32_t now_ms);

/* True while `fp` is on the list with grace left at `now_ms`. */
bool contains(const List& list, const uint8_t fp[FP_LEN], uint32_t now_ms);

/* Drop every entry whose grace has run out. Returns how many were dropped. */
size_t expire(List& list, uint32_t now_ms);

size_t count(const List& list, uint32_t now_ms);

/* Persistence. encode() writes the live entries (grace left > 0) as
 * fingerprint || remaining LE; returns the byte count (0 when empty or
 * cap is too small). decode() merges a stored blob into the list, counting
 * each remaining time from `now_ms` (a longer grace already held wins);
 * false on a malformed length. */
size_t encode(const List& list, uint32_t now_ms, uint8_t* out, size_t cap);
bool   decode(List& list, const uint8_t* blob, size_t len, uint32_t now_ms);

}  /* namespace mesh_revocation */

#endif  /* SECURACV_MESH_REVOCATION_H */
