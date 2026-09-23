/**
 * @file key_at_rest.h
 * @brief Where the identity key sleeps — one policy, stated once.
 *
 * THE QUESTION THIS ANSWERS. A Canary's Ed25519 identity key lives in NVS.
 * Whether that NVS is ciphertext or plaintext on the flash chip is a fact
 * about the silicon AND the build, not something to assume, and every
 * consumer used to answer it differently: the household mesh refuses to
 * persist its shared opera_secret on flash-encryption-off hardware (audit O2,
 * mesh_state.cpp), the identity key was written with no check at all, and one
 * stale comment claimed the opposite of each. This header is where the
 * difference is written down once, so the next reader stops calling one of
 * them a bug.
 *
 * FLASH ENCRYPTION DOES NOT COVER NVS. This is the fact the whole module
 * turns on, and the easy one to get wrong. With flash encryption on, ESP-IDF
 * transparently encrypts only the app, otadata and nvs_keys partitions; an
 * unflagged nvs partition is still written in plaintext, and flagging it
 * `encrypted` does not help — plain NVS then refuses to open it at all
 * (ESP_ERR_NVS_WRONG_ENCRYPTION; IDF v4.4 spi_flash/partition.c and
 * nvs_flash/src/nvs_partition_lookup.cpp; the NVS docs: "NVS is not directly
 * compatible with the ... flash encryption system"). NVS contents are
 * ciphertext only under NVS ENCRYPTION — a separate feature
 * (CONFIG_NVS_ENCRYPTION, keyed from the encrypted nvs_keys partition,
 * secure-initialized by nvs_flash_init()) that in the IDF this tree uses
 * depends on flash encryption being configured in the build. So the key is
 * encrypted at rest only when BOTH are active, and `Facts::nvs_encryption`
 * is the second fact. The Arduino 2.0.17 core's precompiled sdkconfig leaves
 * CONFIG_SECURE_FLASH_ENC_ENABLED unset, so NVS encryption does not exist in
 * that build, and the PIO canary tree's firmware reports the fact as false
 * on every image — on a fused board too, which is the honest answer and the
 * safe direction (under-reporting can only make an opt-in image refuse).
 * The route to encrypted NVS is the arduino-as-IDF-component migration
 * (roadmap item 9), which is also where this fact learns to read true.
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
 *      it promises the key is protected at rest, so unless the key's NVS is
 *      actually encrypted — flash encryption AND NVS encryption, see above;
 *      flash encryption alone is not enough — it must refuse to CREATE the
 *      key (persist) AND refuse to USE one it finds (load), the exact posture
 *      mesh_state already applies to the household secret. Refusing only one
 *      of the two would let a key written by a default image be used by the
 *      image that promised it never would be. Provisioning then fails closed,
 *      on purpose. Under framework = arduino that is EVERY board, fused or
 *      not: the promise cannot be kept by this toolchain, so the image does
 *      not pretend it is.
 *
 *   3. THE LABELS ARE WIRE FIELDS. `wire_label()` is what /api/status, the
 *      health export and the self-manifest carry as `key_at_rest`, and what
 *      the bench checklist greps. A label's meaning never changes; a new tier
 *      gets a new label. The labels name where the key's bytes sit
 *      ("nvs-encrypted" means NVS encryption is active, which implies flash
 *      encryption), never merely which eFuses are burned. The household mesh
 *      and the WAP beacon set answer the same question with their own gates —
 *      this label describes the device's OWN identity key, nothing else.
 *
 * Secure Boot alone does not protect anything at rest: it stops unsigned
 * firmware from running, it does not encrypt flash. So a board with Secure
 * Boot and no encrypted NVS still classifies as plaintext. Nor does the
 * module claim the ladder's Tier 4: that also needs RELEASE-mode flash
 * encryption and JTAG off, neither of which it reads.
 *
 * `Tier::HardwareBound` is reserved for a key the DS/HMAC peripheral holds
 * (roadmap item 18; hardware_root_of_trust.md §5.4, §8 #4). No firmware
 * produces it yet and `classify()` never returns it; it exists so the wire
 * label is decided before the first image that needs it.
 *
 * Board-agnostic and dependency-free so `firmware/tests_host` can exercise
 * every branch — see test_key_at_rest.cpp. Callers own the eFuse reads and
 * NVS; this module owns only the decision and the words the device prints.
 */

#ifndef SECURACV_COMMON_KEY_AT_REST_H
#define SECURACV_COMMON_KEY_AT_REST_H

#include <stdint.h>

namespace key_at_rest {

/** What the device can observe about itself at the moment it asks. */
struct Facts {
  bool flash_encryption = false;  ///< esp_flash_encryption_enabled()
  /// The default NVS partition is encrypted: NVS encryption compiled in
  /// (CONFIG_NVS_ENCRYPTION), flash encryption on, and the default NVS
  /// secure-initialized from an encrypted nvs_keys partition. Always false
  /// under framework = arduino (see the file comment) — the safe direction.
  bool nvs_encryption = false;
  bool secure_boot = false;       ///< esp_secure_boot_enabled(), 0 when absent
  /// SECURACV_REQUIRE_FLASH_ENCRYPTION != 0: the image promises the key is
  /// encrypted at rest, which takes flash encryption AND NVS encryption.
  bool require_fe = false;
};

/** Where the key's bytes actually sit. */
enum class Tier : uint8_t {
  PlaintextNvs = 0,             ///< readable from a flash dump — Tier 0, the accepted default
  EncryptedNvs = 1,             ///< NVS encryption (which requires flash encryption)
  EncryptedNvsSecureBoot = 2,   ///< the same, plus Secure Boot
  HardwareBound = 3,            ///< reserved: DS/HMAC-held key (never produced yet)
};

/** True only when the key's NVS home is ciphertext on the chip. */
inline bool nvs_is_encrypted(const Facts& f) {
  return f.flash_encryption && f.nvs_encryption;
}

/** Rule 3 (and the two caveats): the tier from the facts. Flash encryption
 *  without NVS encryption, and Secure Boot without either, stay plaintext. */
inline Tier classify(const Facts& f) {
  if (!nvs_is_encrypted(f)) return Tier::PlaintextNvs;
  return f.secure_boot ? Tier::EncryptedNvsSecureBoot : Tier::EncryptedNvs;
}

/** The wire field. Lower-case, no spaces, never renamed. */
inline const char* wire_label(Tier t) {
  switch (t) {
    case Tier::PlaintextNvs:           return "plaintext-nvs";
    case Tier::EncryptedNvs:           return "nvs-encrypted";
    case Tier::EncryptedNvsSecureBoot: return "nvs-encrypted+secure-boot";
    case Tier::HardwareBound:          return "hw-bound";
  }
  return "plaintext-nvs";  // unreachable; the safe answer is the weakest one
}

/** The human text for a tier; the boot line carries it when decide() has no
 *  warning or refusal to explain (boot_text()). */
inline const char* console_label(Tier t) {
  switch (t) {
    case Tier::PlaintextNvs:           return "plaintext NVS (Tier 0 default)";
    case Tier::EncryptedNvs:           return "encrypted NVS (flash + NVS encryption)";
    case Tier::EncryptedNvsSecureBoot: return "encrypted NVS (flash + NVS encryption) + Secure Boot";
    case Tier::HardwareBound:          return "hardware-bound (DS/HMAC)";
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
  if (f.require_fe && !nvs_is_encrypted(f)) {
    return { false, false, false,
             "encrypted NVS required by this build but NVS encryption is not "
             "active (flash encryption alone does not cover NVS)" };
  }
  if (!f.flash_encryption) {
    return { true, true, true,
             "identity key at rest in plaintext NVS (Tier 0 default; "
             "docs/security/SECURITY_MODEL.md)" };
  }
  if (!nvs_is_encrypted(f)) {
    return { true, true, true,
             "identity key at rest in plaintext NVS: flash encryption is on "
             "but does not cover NVS, and NVS encryption is not active "
             "(docs/security/SECURITY_MODEL.md)" };
  }
  return { true, true, false, nullptr };
}

/** The boot line's level: "!!" for a refusal, "WARN" for the at-rest
 *  warning, "INFO" otherwise — straight from decide(), never re-derived. */
inline const char* boot_level(const Facts& f) {
  const Decision d = decide(f);
  if (!d.allow_load || !d.allow_persist) return "!!";
  return d.warn_once ? "WARN" : "INFO";
}

/** The boot line's text after the wire label: decide()'s reason when it has
 *  one (a warning or a refusal), else the tier's console label. */
inline const char* boot_text(const Facts& f) {
  const Decision d = decide(f);
  return d.reason ? d.reason : console_label(classify(f));
}

}  // namespace key_at_rest

#endif  // SECURACV_COMMON_KEY_AT_REST_H
