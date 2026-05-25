/*
 * SecuraCV Canary — Mesh-layer NVS persistence — Implementation
 *
 * Device build (ESP32): wraps Arduino Preferences on the canonical
 * "securacv" NVS namespace. Same pattern as ble_scout_key.cpp.
 *
 * Host build (CSI_TEST_HOST_BUILD): deterministic stubs that don't
 * touch any storage. load returns false; save/clear are no-op success.
 */

#include "mesh_state.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>
  #include <Preferences.h>
  #include <esp_flash_encrypt.h>   /* esp_flash_encryption_enabled() */
#endif

namespace mesh_state {

namespace {

#ifndef CSI_TEST_HOST_BUILD
/* Mirrors ble_scout_key.cpp + securacv_audio.cpp — single canonical
 * "securacv" namespace so the same NVS partition entry holds all
 * canary keys. 15-char key budget per ESP32 NVS rules; "opera_secret"
 * is exactly 12. */
constexpr const char* NVS_NAMESPACE = "securacv";
constexpr const char* NVS_KEY_OPERA = "opera_secret";

/* Project invariant (AGENTS.md §"NVS write paths"): persisting the
 * household opera_secret to NVS requires flash encryption to be
 * active. canary-wap's mesh_network.cpp:1053 enforces the same check
 * on its opera_config write/load paths. Devices without FE blown
 * simply cannot store an opera_secret — the user must complete
 * pairing on every boot until the eFuse is committed. */
inline bool flash_encryption_enabled() {
  return esp_flash_encryption_enabled();
}
#endif

}  /* namespace */

bool save_opera_secret(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN]) {
  if (secret == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: succeed silently — tests use in-memory state, not NVS.
   * The flash-encryption gate is a hardware-only concern, not a logic
   * concern; we don't simulate it on host. */
  return true;
#else
  if (!flash_encryption_enabled()) {
    /* AGENTS.md invariant: refuse to persist on FE-off devices.
     * Serial-log so the developer/operator sees WHY pairing won't
     * survive a reboot. log_health would be a circular dep on the
     * witness lib from this lib; Serial is sufficient. */
    Serial.println("[ALERT][mesh_state] refused save_opera_secret — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  const size_t put = prefs.putBytes(NVS_KEY_OPERA, secret,
                                    mesh_crypto::OPERA_SECRET_LEN);
  prefs.end();
  return put == mesh_crypto::OPERA_SECRET_LEN;
#endif
}

bool load_opera_secret(uint8_t out[mesh_crypto::OPERA_SECRET_LEN]) {
  if (out == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: always report "no secret persisted" so tests start
   * in a clean unpaired state. */
  return false;
#else
  if (!flash_encryption_enabled()) {
    /* Match canary-wap mesh_network.cpp:1080 — if FE is off we
     * MUST NOT load any stored secret either. A device with FE
     * unblown but a previously-persisted secret (e.g. the eFuse was
     * disabled after pairing) is in a misconfigured state; report
     * "no secret" so the firmware boots as unpaired. */
    Serial.println("[ALERT][mesh_state] refused load_opera_secret — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    return false;
  }
  const size_t got = prefs.getBytes(NVS_KEY_OPERA, out,
                                    mesh_crypto::OPERA_SECRET_LEN);
  prefs.end();
  return got == mesh_crypto::OPERA_SECRET_LEN;
#endif
}

bool clear_opera_secret() {
#ifdef CSI_TEST_HOST_BUILD
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  /* Preferences::remove returns false when the key is absent OR on a
   * real NVS error (flash corruption, hardware fault). To make this
   * call idempotent across factory-reset semantics WITHOUT swallowing
   * real failures, we check isKey() on the false path:
   *   • remove → true                 ⇒ erased successfully
   *   • remove → false, isKey → false ⇒ already absent, idempotent OK
   *   • remove → false, isKey → true  ⇒ real NVS write failure */
  bool ok = prefs.remove(NVS_KEY_OPERA);
  if (!ok) {
    ok = !prefs.isKey(NVS_KEY_OPERA);
  }
  prefs.end();
  return ok;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * TRUSTED-PEER LIST
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef CSI_TEST_HOST_BUILD
constexpr const char* NVS_KEY_PEERS = "trusted_peers";
constexpr size_t      PEERS_BLOB_MAX = MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN;
#endif

bool save_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]) {
  if (pubkey == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  return true;
#else
  if (!flash_encryption_enabled()) {
    Serial.println("[ALERT][mesh_state] refused save_trusted_peer — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    return false;
  }

  /* Read-modify-write: load current list, dedup, append, store. */
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;

  uint8_t blob[PEERS_BLOB_MAX];
  size_t cur_bytes = 0;
  /* getBytes returning 0 is AMBIGUOUS: key absent OR real read
   * failure (key type mismatch, hardware fault, blob larger than
   * buffer). Treating "failure" as "no peers yet" would silently
   * overwrite the existing list with just our new pubkey — peer-
   * list LOSS. isKey() disambiguates: if the key is absent, this
   * is a legitimate first-add; if present but getBytes returns 0,
   * propagate the read failure. */
  if (prefs.isKey(NVS_KEY_PEERS)) {
    cur_bytes = prefs.getBytes(NVS_KEY_PEERS, blob, sizeof(blob));
    if (cur_bytes == 0 || cur_bytes % mesh_crypto::PUBKEY_LEN != 0) {
      /* Real read failure OR partial-write recovery. Fail loud
       * rather than risk clobbering whatever the user had. */
      prefs.end();
      return false;
    }
  }
  const size_t cur_count = cur_bytes / mesh_crypto::PUBKEY_LEN;

  /* Dedup against existing entries — re-adding the same pubkey is a
   * no-op success, NOT an append that would silently fill the table. */
  for (size_t i = 0; i < cur_count; ++i) {
    if (mesh_crypto::ct_equal(blob + i * mesh_crypto::PUBKEY_LEN,
                              pubkey, mesh_crypto::PUBKEY_LEN)) {
      prefs.end();
      return true;
    }
  }
  if (cur_count >= MAX_TRUSTED_PEERS) {
    prefs.end();
    return false;   /* table full + new pubkey — refuse */
  }

  memcpy(blob + cur_count * mesh_crypto::PUBKEY_LEN,
         pubkey, mesh_crypto::PUBKEY_LEN);
  const size_t new_bytes = (cur_count + 1) * mesh_crypto::PUBKEY_LEN;
  const size_t put = prefs.putBytes(NVS_KEY_PEERS, blob, new_bytes);
  prefs.end();
  return put == new_bytes;
#endif
}

bool load_trusted_peers(uint8_t* out_pubkeys,
                        size_t   out_buf_cap,
                        size_t*  out_count) {
  if (out_pubkeys == nullptr || out_count == nullptr) return false;
  if (out_buf_cap < MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host stub: deterministic empty list. */
  *out_count = 0;
  return true;
#else
  if (!flash_encryption_enabled()) {
    /* Mirror load_opera_secret: refuse to surface persisted peers
     * when FE is off. Boot as if the list were empty. */
    Serial.println("[ALERT][mesh_state] refused load_trusted_peers — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    *out_count = 0;
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    *out_count = 0;
    return false;
  }
  /* isKey() before getBytes() so a real read failure (got==0 with
   * the key present) reports false instead of silently appearing
   * as an empty list. Empty-list-by-design is fine; empty-list-
   * because-NVS-broke is not, and callers can't tell otherwise. */
  size_t got = 0;
  if (prefs.isKey(NVS_KEY_PEERS)) {
    got = prefs.getBytes(NVS_KEY_PEERS, out_pubkeys, out_buf_cap);
    if (got == 0) {
      prefs.end();
      *out_count = 0;
      return false;   /* key present but read failed */
    }
  }
  prefs.end();
  /* Reject malformed blobs (anything not a multiple of PUBKEY_LEN
   * means a partial write happened — don't trust truncated data). */
  if (got % mesh_crypto::PUBKEY_LEN != 0) {
    *out_count = 0;
    return false;
  }
  const size_t n = got / mesh_crypto::PUBKEY_LEN;
  *out_count = (n > MAX_TRUSTED_PEERS) ? MAX_TRUSTED_PEERS : n;
  return true;
#endif
}

bool clear_trusted_peers() {
#ifdef CSI_TEST_HOST_BUILD
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  bool ok = prefs.remove(NVS_KEY_PEERS);
  if (!ok) {
    /* Same isKey() idempotency dance as clear_opera_secret. */
    ok = !prefs.isKey(NVS_KEY_PEERS);
  }
  prefs.end();
  return ok;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * REPLAY COUNTERS
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef CSI_TEST_HOST_BUILD
constexpr const char* NVS_KEY_REPLAY = "replay_ctrs";
constexpr size_t ENTRY_SIZE = mesh_crypto::FINGERPRINT_LEN + sizeof(uint64_t);
#endif

bool save_replay_counters(const ReplayEntry* entries, size_t count) {
#ifdef CSI_TEST_HOST_BUILD
  (void)entries; (void)count;
  return true;
#else
  if (entries == nullptr && count > 0) return false;
  if (count > MAX_REPLAY_ENTRIES) return false;
  if (!esp_flash_encryption_enabled()) return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;

  uint8_t blob[MAX_REPLAY_ENTRIES * ENTRY_SIZE];
  size_t offset = 0;
  for (size_t i = 0; i < count; ++i) {
    memcpy(blob + offset, entries[i].fingerprint, mesh_crypto::FINGERPRINT_LEN);
    offset += mesh_crypto::FINGERPRINT_LEN;
    memcpy(blob + offset, &entries[i].last_counter, sizeof(uint64_t));
    offset += sizeof(uint64_t);
  }

  const size_t put = prefs.putBytes(NVS_KEY_REPLAY, blob, offset);
  prefs.end();
  return put == offset;
#endif
}

bool load_replay_counters(ReplayEntry* out_entries,
                          size_t       out_cap,
                          size_t*      out_count) {
#ifdef CSI_TEST_HOST_BUILD
  if (out_count) *out_count = 0;
  return true;
#else
  if (out_entries == nullptr || out_count == nullptr) return false;
  if (out_cap < MAX_REPLAY_ENTRIES) return false;
  if (!esp_flash_encryption_enabled()) return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;

  if (!prefs.isKey(NVS_KEY_REPLAY)) {
    *out_count = 0;
    prefs.end();
    return true;
  }

  uint8_t blob[MAX_REPLAY_ENTRIES * ENTRY_SIZE];
  const size_t got = prefs.getBytes(NVS_KEY_REPLAY, blob, sizeof(blob));
  prefs.end();

  if (got == 0 || (got % ENTRY_SIZE) != 0) {
    *out_count = 0;
    return got == 0;
  }

  size_t n = got / ENTRY_SIZE;
  if (n > MAX_REPLAY_ENTRIES) n = MAX_REPLAY_ENTRIES;
  for (size_t i = 0; i < n; ++i) {
    const size_t off = i * ENTRY_SIZE;
    memcpy(out_entries[i].fingerprint, blob + off, mesh_crypto::FINGERPRINT_LEN);
    memcpy(&out_entries[i].last_counter, blob + off + mesh_crypto::FINGERPRINT_LEN, sizeof(uint64_t));
  }
  *out_count = n;
  return true;
#endif
}

bool clear_replay_counters() {
#ifdef CSI_TEST_HOST_BUILD
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  bool ok = prefs.remove(NVS_KEY_REPLAY);
  if (!ok) ok = !prefs.isKey(NVS_KEY_REPLAY);
  prefs.end();
  return ok;
#endif
}

}  /* namespace mesh_state */
