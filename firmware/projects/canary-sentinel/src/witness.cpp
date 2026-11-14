#include "canary/witness.h"

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>          // load_or_generate_keypair(): IDF's own answer for "nothing stored" (sweep F362)
#include <cstring>

#include <Ed25519.h>
#include <esp_random.h>
#include <bootloader_random.h>  // early-entropy source for the first-boot identity keygen
#include <mbedtls/sha256.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/runtime_config.h"
#include "identity/device_signature.h"  // shared signer (common/identity)
#include "identity/key_draw_check.h"    // a flat first-boot key draw is refused (sweep F38)
#include "witness/witness_chain.h"       // canonical chain core (common/witness)

namespace canary::witness {

namespace {

// Chain construction and domain strings come from the shared canonical
// core (witness/witness_chain.h) — one definition across every firmware,
// verified off-device by tools/verify_witness_log.py.
constexpr const char* DOMAIN_FINGERPRINT = "securacv:pubkey:fingerprint";
constexpr const char* DOMAIN_PAYLOAD     = WC_DOMAIN_PAYLOAD;

// NVS layout (namespace "securacv", shared with runtime_config):
//   privkey    32 B  Ed25519 private key (first boot generates)
//   chain_head 32 B  current hash-chain head
//   chain_len  u32   number of chained events
constexpr const char* NVS_NAMESPACE  = "securacv";
constexpr const char* KEY_PRIV       = "privkey";
constexpr const char* KEY_CHAIN_HEAD = "chain_head";
constexpr const char* KEY_CHAIN_LEN  = "chain_len";

uint8_t  s_priv[32]   = {};
uint8_t  s_pub[32]    = {};
uint8_t  s_head[32]   = {};
uint32_t s_length     = 0;
bool     s_ready      = false;

// Domain-separated SHA-256 — thin local alias over the shared core's
// wc_sha256_domain (witness/witness_chain.h) kept for the fingerprint
// and payload hashes below. False, with out zeroed, when the hash did not
// run (sweep F455); every caller refuses what it was for.
[[nodiscard]] bool sha256_domain(const char* domain, const uint8_t* data, size_t n,
                                 uint8_t out[32]) {
  return wc_sha256_domain(domain, data, n, out);
}

// Reliable scrub for key material (volatile pointer defeats dead-store
// elimination — same helper the pseudonym module uses).
void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
}

// The identity key, fail closed (sweep F362, the flagship's F360 rule):
// what NVS holds is asked of IDF before Preferences reads it, because
// Preferences' getBytesLength() answers 0 alike for a key never stored and
// a lookup that failed. Only IDF's ESP_ERR_NVS_NOT_FOUND (no namespace yet,
// or no key in it) draws a new key. Every other answer, a stored key of
// another length, a read that does not return all 32 bytes and a stored key
// of 32 zero bytes disable signing for this boot and write nothing, the path
// a failed open already takes: the buffer is scrubbed and the stored entry
// left as it was, so the next boot that can read it signs with it again
// (one exception: an entry whose data CRC fails is erased by IDF in that
// same read, so a later boot finds none and draws a new key; the
// fingerprint alert is put off, not prevented).
// Before, each of those drew a new key and stored it over the old one, which
// Home Assistant reads as "Fingerprint changed without rotation" and the
// owner cannot tell from tampering, and the old identity was gone for good.
bool load_or_generate_keypair() {
  size_t stored = 0;
  nvs_handle_t h;
  esp_err_t probe = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (probe == ESP_OK) {
    probe = nvs_get_blob(h, KEY_PRIV, nullptr, &stored);
    nvs_close(h);
  }
  if (probe != ESP_OK && probe != ESP_ERR_NVS_NOT_FOUND) {
    secure_zero(s_priv, sizeof(s_priv));
    canary::dbg_serial().printf(
        "[WITNESS] NVS could not say whether a key is stored (%s) — signing disabled this boot.\n",
        esp_err_to_name(probe));
    return false;
  }
  const bool absent = (probe == ESP_ERR_NVS_NOT_FOUND);
  if (!absent && stored != sizeof(s_priv)) {
    secure_zero(s_priv, sizeof(s_priv));
    canary::dbg_serial().printf(
        "[WITNESS] Stored key is %u bytes, not 32 — signing disabled this boot.\n",
        (unsigned)stored);
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/!absent)) {
    secure_zero(s_priv, sizeof(s_priv));
    log_line("WITNESS", "NVS unavailable — signing disabled this boot.");
    return false;
  }

  if (!absent) {
    const size_t got = prefs.getBytes(KEY_PRIV, s_priv, sizeof(s_priv));
    prefs.end();
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(s_priv); i++) any |= s_priv[i];
    if (got != sizeof(s_priv) || any == 0) {
      // A refused read can leave the buffer as it was or partly written.
      secure_zero(s_priv, sizeof(s_priv));
      if (got != sizeof(s_priv)) {
        canary::dbg_serial().printf(
            "[WITNESS] Stored key could not be read (%u of 32 bytes) — signing disabled this boot.\n",
            (unsigned)got);
      } else {
        log_line("WITNESS", "Stored key is 32 zero bytes — signing disabled this boot.");
      }
      return false;
    }
    Ed25519::derivePublicKey(s_pub, s_priv);
    return true;
  }

  // First boot: 32 bytes from the hardware RNG, exactly like the
  // canary-wap tree's generate_keypair. This runs during witness init, before
  // WiFi/BT start, so the RNG has no RF entropy source yet — gate the one-time
  // identity draw with bootloader_random_enable()/_disable() to seed it
  // properly (ESP-IDF's documented early-entropy pattern). The RF subsystem
  // seeds the RNG for every later draw. The draw is looked at before it is
  // stored (sweep F38, key_draw_check.h): a flat one (every byte equal, or
  // too few or too many one bits) disables signing for this boot and writes
  // nothing, so the next boot draws again.
  bootloader_random_enable();
  esp_fill_random(s_priv, sizeof(s_priv));
  const bool flat = key_draw_check::is_flat(s_priv);
  bootloader_random_disable();
  if (flat) {
    secure_zero(s_priv, sizeof(s_priv));
    prefs.end();
    log_line("WITNESS", "Key draw came out flat — no key stored, signing disabled this boot.");
    return false;
  }
  if (prefs.putBytes(KEY_PRIV, s_priv, sizeof(s_priv)) != sizeof(s_priv)) {
    // An unpersisted key would rotate on every reboot, and each
    // rotation reads as a "Fingerprint changed without rotation"
    // security alert in HA. Unsigned publishes degrade gracefully
    // there; a flapping identity does not — so fail closed.
    log_line("WITNESS",
             "Key persist FAILED — signing disabled (an ephemeral key "
             "would flap the fingerprint every reboot).");
    secure_zero(s_priv, sizeof(s_priv));
    prefs.end();
    return false;
  }
  prefs.end();
  log_line("WITNESS", "Generated new Ed25519 identity (first boot).");
  Ed25519::derivePublicKey(s_pub, s_priv);
  return true;
}

// False only when no chain was read and the genesis head did not compute
// (sweep F455): s_head is then zero, and init() refuses the boot's chain
// rather than chain records from it.
bool load_or_start_chain() {
  Preferences prefs;
  bool loaded = false;
  if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    if (prefs.getBytesLength(KEY_CHAIN_HEAD) == sizeof(s_head) &&
        prefs.getBytes(KEY_CHAIN_HEAD, s_head, sizeof(s_head)) == sizeof(s_head)) {
      s_length = prefs.getULong(KEY_CHAIN_LEN, 0);
      loaded = true;
    }
    prefs.end();
  }
  if (!loaded) {
    // Canonical genesis binds the chain to the device's stable id
    // (matches canary-wap and tools/verify_witness_log.py). Devices
    // upgraded from the older pubkey-bound genesis keep their stored
    // head and continue forward — no external verifier recomputes
    // sense's historical links (HA verifies the signed head envelope).
    s_length = 0;
    return wc_genesis(canary::cfg::get().device_id, s_head);
  }
  return true;
}

void persist_chain() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putBytes(KEY_CHAIN_HEAD, s_head, sizeof(s_head));
  prefs.putULong(KEY_CHAIN_LEN, s_length);
  prefs.end();
}

}  // namespace

bool init() {
  if (s_ready) return true;

  if (!load_or_generate_keypair()) {
    s_ready = false;
    return false;
  }

  // Fingerprint: wap formula — SHA256(domain || 0x00 || pubkey)[0..8] hex.
  // A fingerprint or a genesis head the hash did not compute (sweep F455)
  // disables signing for this boot, the path a refused key takes: the key
  // is scrubbed before the signer sees it, and no chain head or length is
  // stored. (On a first boot the new key is already stored by then; the
  // next boot loads it and computes both again.)
  uint8_t fp_hash[32];
  const bool fp_ok = sha256_domain(DOMAIN_FINGERPRINT, s_pub, sizeof(s_pub), fp_hash);
  if (!fp_ok) {
    secure_zero(s_priv, sizeof(s_priv));
    log_line("WITNESS", "Fingerprint not computed (SHA-256 failed) — signing disabled this boot.");
    return false;
  }
  if (!load_or_start_chain()) {
    secure_zero(s_priv, sizeof(s_priv));
    log_line("WITNESS", "Genesis head not computed (SHA-256 failed) — signing disabled this boot.");
    return false;
  }
  char fp_hex[17];
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) {
    fp_hex[2 * i]     = H[(fp_hash[i] >> 4) & 0xF];
    fp_hex[2 * i + 1] = H[(fp_hash[i] >> 0) & 0xF];
  }
  fp_hex[16] = '\0';

  device_signature::init(s_priv, s_pub, canary::cfg::get().device_id, fp_hex);
  // device_signature keeps its own copy; scrub ours so exactly one copy
  // of the private key stays resident.
  secure_zero(s_priv, sizeof(s_priv));
  s_ready = true;

  log_header("WITNESS");
  canary::dbg_serial().printf(
      "Ed25519 ready  fp=%s  chain_len=%lu\n",
      fp_hex, (unsigned long)s_length);
  // The full public key, which is public data: the manual pin in Home
  // Assistant takes these 64 hex characters, and the serial console is
  // this product's only way to show them without the broker in the path.
  // Lowercase, the same string the health publish carries as public_key.
  // The line is 79 characters, so it does not wrap on an 80-column console.
  canary::dbg_serial().printf("Ed25519 pubkey %s\n",
                              device_signature::pubkey_hex());
  return true;
}

bool ready() { return s_ready; }

bool sign_event_envelope(uint32_t             seq,
                         const SentinelClaim& claim,
                         uint32_t             bucket_uptime_s,
                         char*                out,
                         size_t               cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!s_ready) return false;

  char sig_b64[device_signature::SIG_B64URL_CAP];
  if (!device_signature::sign_sentinel(seq, claim.event, claim.level,
                                       claim.confidence, claim.anomaly,
                                       claim.occupancy, claim.range,
                                       claim.modality_bits, bucket_uptime_s,
                                       sig_b64, sizeof(sig_b64))) {
    return false;
  }

  const int n = snprintf(out, cap,
                         ",\"v\":%d,\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"",
                         device_signature::SCHEMA_V,
                         device_signature::ALG_NAME,
                         device_signature::fingerprint_hex(),
                         sig_b64);
  if (n <= 0 || (size_t)n >= cap) {
    // Never emit a half-built envelope — the truncated JSON would break
    // the whole event payload downstream.
    out[0] = '\0';
    return false;
  }
  return true;
}

void chain_advance(uint32_t             seq,
                   const SentinelClaim& claim,
                   uint32_t             bucket_uptime_s) {
  if (!s_ready) return;

  char canonical[256];
  const size_t n = device_signature::build_sentinel_canonical(
      seq, claim.event, claim.level, claim.confidence, claim.anomaly,
      claim.occupancy, claim.range, claim.modality_bits, bucket_uptime_s,
      device_signature::device_id(), canonical, sizeof(canonical));
  if (n == 0) return;

  // A hash the engine did not compute, the payload's or the link's, chains
  // nothing (sweep F455): the head and the length stay as they were and
  // nothing is persisted, so the next record links from the same head.
  uint8_t payload_hash[32];
  if (!sha256_domain(DOMAIN_PAYLOAD,
                     reinterpret_cast<const uint8_t*>(canonical), n, payload_hash)) {
    return;
  }

  // Canonical advance (witness_chain.h): seq and the coarse time bucket
  // are part of the hash, so a record cannot be silently renumbered or
  // time-shifted. This replaced the older head||payload-only form, which
  // omitted both (the divergence flagged in the witness audit).
  uint8_t new_head[32];
  const bool linked = wc_chain_advance(s_head, payload_hash, seq, bucket_uptime_s, new_head);
  secure_zero(payload_hash, sizeof(payload_hash));
  if (!linked) return;
  memcpy(s_head, new_head, sizeof(s_head));
  s_length++;

  persist_chain();
}

uint32_t chain_length() { return s_length; }
const uint8_t* chain_head() { return s_head; }

} // namespace canary::witness
