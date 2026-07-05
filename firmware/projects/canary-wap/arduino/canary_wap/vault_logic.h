/*
 * SecuraCV Canary WAP — pure sealed-snapshot vault decisions (host-testable)
 *
 * Arduino-free: stdint/stddef/string only. The camera capture, crypto, and
 * SD work live in vault_snapshot.cpp; every branchy DECISION (should this
 * trigger capture?) and every BYTE-EXACT format (the .svlt file header, the
 * ring filename) lives here so a host g++ run (test_vault_logic.cpp) and the
 * off-device unlock tool (tools/unseal_snapshot.py) can pin them.
 *
 * What this feature is: an OPT-IN, per-trigger, write-only escrow of camera
 * frames on life-safety acoustic events (T3 smoke / T4 CO / glass break).
 * The device stores only the operator's X25519 PUBLIC key and seals each
 * JPEG with an ephemeral sealed box (X25519 + HKDF-SHA256 +
 * ChaCha20-Poly1305) — the device is structurally unable to read a sealed
 * frame back. Everything defaults OFF; with no registered key nothing is
 * ever captured. This is the device-side analog of the witness kernel's
 * break-glass evidence vault.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_VAULT_LOGIC_H
#define SECURACV_VAULT_LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace vault_logic {

/* ── Constants ──────────────────────────────────────────────────────── */

/* On-disk header is exactly this many bytes; it is also the AEAD AAD, so
 * trigger/time-bucket/key-id/ephemeral-key/nonce/length are all bound to
 * the ciphertext — a swapped or edited header fails the tag check. */
constexpr size_t   HEADER_SIZE     = 64;
constexpr uint8_t  FORMAT_VERSION  = 1;
constexpr size_t   KEY_ID_SIZE     = 8;   /* first 8 B of SHA-256(pubkey) */
constexpr size_t   PUBKEY_SIZE     = 32;  /* X25519 */
constexpr size_t   NONCE_SIZE      = 12;  /* ChaCha20-Poly1305 */
constexpr size_t   TAG_SIZE        = 16;

/* Ring bound + size cap: newest KEEP_FILES sealed frames are retained
 * (datamgmt::rotate_dir enforces it); a single ciphertext larger than
 * MAX_CIPHERTEXT is rejected at capture time (XGA JPEG is ~100-300 KB). */
constexpr uint32_t KEEP_FILES      = 20;
constexpr uint32_t MAX_CIPHERTEXT  = 512UL * 1024UL;

/* Default per-trigger cooldown. Alarm cadences re-fire continuously for
 * minutes; one sealed frame per minute per trigger bounds SD wear. */
constexpr uint16_t DEFAULT_COOLDOWN_S = 60;

/* ── Triggers ───────────────────────────────────────────────────────── */

enum class Trigger : uint8_t {
  NONE     = 0,
  T3_SMOKE = 1,
  T4_CO    = 2,
  GLASS    = 3,
  TEST     = 9,   /* manual /api/vault/test capture */
};

/* Short tag used in filenames, witness-event state_name, and the UI. */
inline const char* trigger_tag(Trigger t) {
  switch (t) {
    case Trigger::T3_SMOKE: return "smoke";
    case Trigger::T4_CO:    return "co";
    case Trigger::GLASS:    return "glass";
    case Trigger::TEST:     return "test";
    default:                return "none";
  }
}

inline bool trigger_valid(uint8_t raw) {
  return raw == (uint8_t)Trigger::T3_SMOKE || raw == (uint8_t)Trigger::T4_CO ||
         raw == (uint8_t)Trigger::GLASS    || raw == (uint8_t)Trigger::TEST;
}

/* ── Capture decision ───────────────────────────────────────────────── */

struct VaultConfig {
  bool     t3_enabled;
  bool     t4_enabled;
  bool     glass_enabled;
  uint16_t cooldown_s;
};

enum class Decision : uint8_t {
  CAPTURE          = 0,
  SKIP_DISABLED    = 1,  /* trigger not opted in */
  SKIP_NO_KEY      = 2,  /* no operator public key registered */
  SKIP_NO_SD       = 3,
  SKIP_NO_CAMERA   = 4,
  SKIP_QR_BUSY     = 5,  /* QR scan owns the sensor configuration */
  SKIP_WORKER_BUSY = 6,  /* a seal is already in flight */
  SKIP_COOLDOWN    = 7,
  SKIP_BAD_TRIGGER = 8,
};

inline const char* decision_name(Decision d) {
  switch (d) {
    case Decision::CAPTURE:          return "capture";
    case Decision::SKIP_DISABLED:    return "disabled";
    case Decision::SKIP_NO_KEY:      return "no_key";
    case Decision::SKIP_NO_SD:       return "no_sd";
    case Decision::SKIP_NO_CAMERA:   return "no_camera";
    case Decision::SKIP_QR_BUSY:     return "qr_busy";
    case Decision::SKIP_WORKER_BUSY: return "worker_busy";
    case Decision::SKIP_COOLDOWN:    return "cooldown";
    default:                         return "bad_trigger";
  }
}

/* Fail-closed decision table. ORDER MATTERS and is part of the contract:
 * hard preconditions (key, SD, camera, sensor contention, single worker)
 * are checked before the per-trigger opt-in and cooldown, so a TEST capture
 * exercises the exact real-capture path (it bypasses only the opt-in and
 * the cooldown — an operator pressing "Test" is explicit intent).
 * Wrap-safe uint32 cooldown math (millis() wraps every ~49.7 days). */
inline Decision capture_decision(Trigger t, const VaultConfig& cfg,
                                 bool has_pubkey, bool sd_ok, bool camera_ok,
                                 bool qr_active, bool worker_busy,
                                 uint32_t now_ms, uint32_t last_capture_ms,
                                 bool has_last_capture) {
  if (!trigger_valid((uint8_t)t))            return Decision::SKIP_BAD_TRIGGER;
  if (!has_pubkey)                           return Decision::SKIP_NO_KEY;
  if (!sd_ok)                                return Decision::SKIP_NO_SD;
  if (!camera_ok)                            return Decision::SKIP_NO_CAMERA;
  if (qr_active)                             return Decision::SKIP_QR_BUSY;
  if (worker_busy)                           return Decision::SKIP_WORKER_BUSY;

  if (t == Trigger::TEST)                    return Decision::CAPTURE;

  const bool enabled = (t == Trigger::T3_SMOKE && cfg.t3_enabled) ||
                       (t == Trigger::T4_CO    && cfg.t4_enabled) ||
                       (t == Trigger::GLASS    && cfg.glass_enabled);
  if (!enabled)                              return Decision::SKIP_DISABLED;

  if (has_last_capture &&
      (uint32_t)(now_ms - last_capture_ms) < (uint32_t)cfg.cooldown_s * 1000UL) {
    return Decision::SKIP_COOLDOWN;
  }
  return Decision::CAPTURE;
}

/* ── .svlt header (byte-exact, little-endian) ───────────────────────────
 *
 * offset  size  field
 *      0     4  magic "SVLT"
 *      4     1  version (1)
 *      5     1  trigger (Trigger)
 *      6     1  time_bucket (0..143 — 10-minute bucket; the ONLY time
 *               information stored, matching the chokepoint's coarsening)
 *      7     1  reserved (0)
 *      8     8  recipient key id (first 8 B of SHA-256(operator pubkey))
 *     16    32  ephemeral X25519 public key
 *     48    12  ChaCha20-Poly1305 nonce
 *     60     4  ciphertext length (u32 LE, excludes the trailing 16 B tag)
 *
 * File = header || ciphertext || tag. AAD = header[0..64].
 */

struct SealHeader {
  uint8_t  trigger;
  uint8_t  time_bucket;
  uint8_t  key_id[KEY_ID_SIZE];
  uint8_t  ephemeral_pub[PUBKEY_SIZE];
  uint8_t  nonce[NONCE_SIZE];
  uint32_t ct_len;
};

inline void header_build(const SealHeader& h, uint8_t out[HEADER_SIZE]) {
  memset(out, 0, HEADER_SIZE);
  out[0] = 'S'; out[1] = 'V'; out[2] = 'L'; out[3] = 'T';
  out[4] = FORMAT_VERSION;
  out[5] = h.trigger;
  out[6] = h.time_bucket;
  out[7] = 0;  /* reserved */
  memcpy(out + 8,  h.key_id,        KEY_ID_SIZE);
  memcpy(out + 16, h.ephemeral_pub, PUBKEY_SIZE);
  memcpy(out + 48, h.nonce,         NONCE_SIZE);
  out[60] = (uint8_t)(h.ct_len);
  out[61] = (uint8_t)(h.ct_len >> 8);
  out[62] = (uint8_t)(h.ct_len >> 16);
  out[63] = (uint8_t)(h.ct_len >> 24);
}

/* Returns false (and leaves *h unspecified) on any malformed field —
 * wrong magic/version, unknown trigger, out-of-range bucket, oversized or
 * zero ciphertext length. Fail-closed: an unparseable file is reported as
 * corrupt, never partially trusted. */
inline bool header_parse(const uint8_t in[HEADER_SIZE], SealHeader* h) {
  if (in[0] != 'S' || in[1] != 'V' || in[2] != 'L' || in[3] != 'T') return false;
  if (in[4] != FORMAT_VERSION)                                      return false;
  if (!trigger_valid(in[5]))                                        return false;
  if (in[6] > 143)                                                  return false;
  h->trigger     = in[5];
  h->time_bucket = in[6];
  memcpy(h->key_id,        in + 8,  KEY_ID_SIZE);
  memcpy(h->ephemeral_pub, in + 16, PUBKEY_SIZE);
  memcpy(h->nonce,         in + 48, NONCE_SIZE);
  h->ct_len = (uint32_t)in[60] | ((uint32_t)in[61] << 8) |
              ((uint32_t)in[62] << 16) | ((uint32_t)in[63] << 24);
  if (h->ct_len == 0 || h->ct_len > MAX_CIPHERTEXT)                 return false;
  return true;
}

/* ── Ring filename: seal_<seq8>_<tag>.svlt ──────────────────────────── */

/* out must hold >= 32 bytes ("seal_00000000_smoke.svlt" = 25 + NUL). */
inline void filename_build(uint32_t seq, Trigger t, char* out, size_t out_len) {
  snprintf(out, out_len, "seal_%08lu_%s.svlt",
           (unsigned long)(seq % 100000000UL), trigger_tag(t));
}

/* Parses "seal_<8 digits>_<tag>.svlt". Returns false on anything else. */
inline bool filename_parse(const char* name, uint32_t* seq, Trigger* t) {
  if (!name || strncmp(name, "seal_", 5) != 0) return false;
  uint32_t s = 0;
  for (int i = 0; i < 8; i++) {
    char c = name[5 + i];
    if (c < '0' || c > '9') return false;
    s = s * 10 + (uint32_t)(c - '0');
  }
  if (name[13] != '_') return false;
  const char* tag = name + 14;
  const char* dot = strchr(tag, '.');
  if (!dot || strcmp(dot, ".svlt") != 0) return false;
  size_t tag_len = (size_t)(dot - tag);
  Trigger found = Trigger::NONE;
  static const Trigger ALL[] = {Trigger::T3_SMOKE, Trigger::T4_CO,
                                Trigger::GLASS, Trigger::TEST};
  for (Trigger cand : ALL) {
    const char* ct = trigger_tag(cand);
    if (strlen(ct) == tag_len && strncmp(tag, ct, tag_len) == 0) {
      found = cand;
      break;
    }
  }
  if (found == Trigger::NONE) return false;
  *seq = s;
  *t   = found;
  return true;
}

}  // namespace vault_logic

#endif  // SECURACV_VAULT_LOGIC_H
