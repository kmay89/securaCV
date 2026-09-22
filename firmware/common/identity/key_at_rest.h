/**
 * @file key_at_rest.h
 * @brief Where the identity key sleeps — one policy, stated once.
 *
 * THE QUESTION THIS ANSWERS. A Canary's Ed25519 identity key lives in NVS.
 * Whether that NVS is ciphertext or plaintext on the flash chip is a fact
 * about the SILICON (the flash-encryption eFuse), not about the firmware, and
 * every consumer used to answer it differently: the household mesh refuses to
 * persist its shared opera_secret on flash-encryption-off hardware (audit O2,
 * mesh_state.cpp), the identity key was written with no check at all, and one
 * stale comment claimed the opposite of each. This header is where the
 * difference is written down once, so the next reader stops calling one of
 * them a bug.
 *
 * THE THREE RULES
 *
 *   1. THE DEFAULT NEVER REFUSES. Tier 0 of the root-of-trust ladder
 *      (docs/design/hardware_root_of_trust.md §5.1) is "Ed25519 identity in
 *      NVS", and decisions §8 #1 and #3 keep the default Canary at Tiers 0–2:
 *      no eFuse is ever burned, the device stays un-brickable, and the
 *      at-rest exposure is a documented trade
 *      (docs/security/SECURITY_MODEL.md, "Physical extraction and the
 *      flash-encryption default"). A default build therefore always allows
 *      the key to be stored and loaded, and says so on one boot line. A
 *      default that halted on ordinary hardware would strand every dev board
 *      and contradict the accepted design.
 *
 *   2. THE OPT-IN REFUSES SYMMETRICALLY. An image built with
 *      `SECURACV_REQUIRE_FLASH_ENCRYPTION=1` is a Tier 3/4 image (§8 #3, #4):
 *      it promises the key is protected at rest, so on hardware where flash
 *      encryption is not active it must refuse to CREATE the key (persist) AND
 *      refuse to USE one it finds (load) — the exact posture mesh_state
 *      already applies to the household secret. Refusing only one of the two
 *      would let a key written by a default image be used by the image that
 *      promised it never would be. Provisioning then fails closed, on purpose.
 *
 *   3. THE LABELS ARE WIRE FIELDS. `wire_label()` is what /api/status, the
 *      health export and the self-manifest carry as `key_at_rest`, and what
 *      the bench checklist greps. A label's meaning never changes; a new tier
 *      gets a new label. The household mesh and the WAP beacon set answer the
 *      same question with their own gates — this label describes the device's
 *      OWN identity key, nothing else.
 *
 * Secure Boot alone does not protect anything at rest: it stops unsigned
 * firmware from running, it does not encrypt flash. So a board with Secure
 * Boot and no flash encryption still classifies as plaintext.
 *
 * `Tier::HardwareBound` is reserved for a key the DS/HMAC peripheral holds
 * (roadmap item 18; hardware_root_of_trust.md §5.4, §8 #4). No firmware
 * produces it yet and `classify()` never returns it; it exists so the wire
 * label is decided before the first image that needs it.
 *
 * Board-agnostic and dependency-free so `firmware/tests_host` can exercise
 * every branch — see test_key_at_rest.cpp. Callers own the eFuse reads and
 * NVS; this module owns only the decision.
 */

#ifndef SECURACV_COMMON_KEY_AT_REST_H
#define SECURACV_COMMON_KEY_AT_REST_H

#include <stdint.h>

namespace key_at_rest {

/** What the device can observe about itself at the moment it asks. */
struct Facts {
  bool flash_encryption = false;  ///< esp_flash_encryption_enabled()
  bool secure_boot = false;       ///< esp_secure_boot_enabled(), 0 when absent
  bool require_fe = false;        ///< SECURACV_REQUIRE_FLASH_ENCRYPTION != 0
};

/** The rung of the root-of-trust ladder the key actually sits on. */
enum class Tier : uint8_t {
  PlaintextNvs = 0,              ///< Tier 0 — the accepted default
  FlashEncrypted = 1,            ///< Tier 3 — dev-mode flash encryption
  FlashEncryptedSecureBoot = 2,  ///< Tier 4 — release FE + Secure Boot v2
  HardwareBound = 3,             ///< reserved: DS/HMAC-held key (never produced yet)
};

/** Rule 3 (and the Secure-Boot-alone caveat): the tier from the facts. */
inline Tier classify(const Facts& f) {
  if (!f.flash_encryption) return Tier::PlaintextNvs;
  return f.secure_boot ? Tier::FlashEncryptedSecureBoot : Tier::FlashEncrypted;
}

/** The wire field. Lower-case, no spaces, never renamed. */
inline const char* wire_label(Tier t) {
  switch (t) {
    case Tier::PlaintextNvs:             return "plaintext-nvs";
    case Tier::FlashEncrypted:           return "flash-encrypted";
    case Tier::FlashEncryptedSecureBoot: return "flash-encrypted+secure-boot";
    case Tier::HardwareBound:            return "hw-bound";
  }
  return "plaintext-nvs";  // unreachable; the safe answer is the weakest one
}

/** The human line for the console card and the boot log. */
inline const char* console_label(Tier t) {
  switch (t) {
    case Tier::PlaintextNvs:             return "plaintext NVS (Tier 0 default)";
    case Tier::FlashEncrypted:           return "flash-encrypted NVS (Tier 3)";
    case Tier::FlashEncryptedSecureBoot: return "flash-encrypted NVS + Secure Boot (Tier 4)";
    case Tier::HardwareBound:            return "hardware-bound (DS/HMAC)";
  }
  return "plaintext NVS (Tier 0 default)";
}

/** What the caller may do with the key, and what it should say about it. */
struct Decision {
  bool allow_persist;   ///< may a freshly generated key be written to NVS
  bool allow_load;      ///< may a stored key be read back and used
  bool warn_once;       ///< print the one-line at-rest warning this boot
  const char* reason;   ///< nullptr when there is nothing to explain
};

/** The whole decision — rules 1 and 2. */
inline Decision decide(const Facts& f) {
  if (f.require_fe && !f.flash_encryption) {
    return { false, false, false,
             "flash encryption required by this build but not active" };
  }
  if (!f.flash_encryption) {
    return { true, true, true,
             "identity key at rest in plaintext NVS (Tier 0 default; "
             "docs/security/SECURITY_MODEL.md)" };
  }
  return { true, true, false, nullptr };
}

}  // namespace key_at_rest

#endif  // SECURACV_COMMON_KEY_AT_REST_H
