/**
 * @file vault_snapshot.h
 * @brief Sealed-snapshot vault: opt-in, event-triggered camera frames,
 *        encrypted so the device cannot read them back (write-only escrow).
 *
 * Device-side analog of the witness kernel's break-glass evidence vault:
 * on a life-safety acoustic trigger (T3 smoke / T4 CO / glass break — each
 * individually opted in, ALL OFF by default) the device captures one JPEG
 * and seals it to /sd/VAULT with an X25519 sealed box (ephemeral ECDH →
 * HKDF-SHA256 → ChaCha20-Poly1305) against the operator's registered
 * PUBLIC key. The private key never touches the device; review happens
 * off-device via tools/unseal_snapshot.py. With no key registered, nothing
 * is ever captured.
 *
 * Threading contract (house rules):
 *   - request_capture()/poll_completion(): MAIN LOOP TASK ONLY. The
 *     decision runs inline (pure vault_logic); the blocking capture+seal
 *     runs on a one-shot worker task; the loop adopts the result and emits
 *     the frame_sealed witness event (chokepoint is loop-single-threaded).
 *   - set_*/list/delete/validate: HTTP task is fine (NVS + SD reads; the
 *     pubkey/config handoff to captures is copied into the job on the loop
 *     at request time, so no cross-task tearing).
 *
 * File format + decision table: vault_logic.h (host-tested).
 */

#ifndef SECURACV_VAULT_SNAPSHOT_H
#define SECURACV_VAULT_SNAPSHOT_H

#include "build_config.h"

#if FEATURE_VAULT_SNAPSHOT

#include <stdint.h>
#include <stddef.h>

#include "vault_logic.h"

namespace vault_snapshot {

/* Load persisted config + key from NVS. Call once in setup() (after NVS is
 * up; skipped in safe mode by the caller). */
void init();

/* Loop-task-only. The .ino supplies the states it owns (camera init flag,
 * QR-scan activity, SD availability). Returns the decision so callers can
 * log it; on CAPTURE the worker has been spawned and the per-trigger
 * cooldown stamped. */
vault_logic::Decision request_capture(vault_logic::Trigger t,
                                      bool camera_ok, bool qr_active,
                                      bool sd_ok);

/* Loop-task-only. Adopts a finished worker: emits the frame_sealed witness
 * event, health-logs the outcome, and rotates /VAULT to the newest
 * vault_logic::KEEP_FILES entries. */
void poll_completion();

/* Key management. set_pubkey_hex expects exactly 64 lowercase/uppercase hex
 * chars (32-byte X25519 public key); persists to NVS and computes the
 * 8-byte key id. clear_pubkey wipes the key AND forces every trigger off
 * (a vault without a recipient must not arm). */
bool set_pubkey_hex(const char* hex64);
void clear_pubkey();
bool has_pubkey();
/* First 16 hex chars of SHA-256(pubkey); out[17]; "" when no key. */
void key_id_hex(char* out);

/* Trigger config (persisted). Cooldown clamped to [10, 3600] s. */
vault_logic::VaultConfig get_config();
void set_config(const vault_logic::VaultConfig& cfg);

bool worker_busy();

/* Listing / management for the API handlers. */
struct ItemInfo {
  char     name[32];
  uint8_t  trigger;      /* vault_logic::Trigger */
  uint8_t  time_bucket;  /* from the file header; 255 if unreadable */
  uint32_t size;         /* whole file size in bytes */
};
/* Fills up to max_items entries from /sd/VAULT; returns the count. */
int list_items(ItemInfo* out, int max_items);
/* True iff name parses as a vault filename (no traversal possible). */
bool validate_name(const char* name);
bool delete_item(const char* name);

}  // namespace vault_snapshot

#endif  /* FEATURE_VAULT_SNAPSHOT */
#endif  /* SECURACV_VAULT_SNAPSHOT_H */
