/*
 * SecuraCV Canary — BLE OTA header policy (host-testable core)
 *
 * The pure half of ble_ota.cpp: parsing the BEGIN payloads, rebuilding the
 * canonical message the protocol-v2 signature covers, and the
 * accept / refuse / needs-break-glass decision. No Arduino or ESP-IDF
 * includes. The Ed25519 primitive is injected (Crypto's Ed25519::verify on
 * the device, OpenSSL in tests_host/test_ble_ota_policy.cpp) so both sides
 * run the same bytes, and the version-floor comparison is the pull
 * engine's own securacv_ota_update_decision() — one anti-rollback rule for
 * every install channel.
 *
 * ── Protocol v2 header (BEGIN_V2, command 0x03) ────────────────────────────
 *
 *   offset  size  field
 *   0       2     magic         0x53 0x43 ("SC")
 *   2       1     hdr_version   0x02
 *   3       1     reserved      0x00 (must be zero)
 *   4       32    product       product id, NUL-terminated, zero-padded
 *   36      32    version       semantic version, NUL-terminated, zero-padded
 *   68      4     image_size    uint32 little-endian
 *   72      32    sha256        SHA-256 of the image
 *   104     64    signature     Ed25519 over the canonical message below
 *   ──────────────
 *   168 bytes total (static_assert'd; the BEGIN_V2 write is 1 + 168 bytes)
 *
 * Canonical message — domain-separated, and the same NUL-separated field
 * convention as the manifest signature (securacv_ota_build_manifest_message
 * / ota_release.py manifest_signed_message), so the release tooling signs
 * both with one key and one helper:
 *
 *   "scv-ble-ota-v2\0" product "\0" version "\0" size-as-decimal "\0"
 *   sha256hex-lowercase "\0"
 *
 * The device rebuilds these bytes from the PARSED fields, never from the
 * raw wire bytes, so everything the policy acts on — the product binding,
 * the version the anti-rollback floor is checked against, the size and
 * digest the stream is checked against — is under the release signature.
 * The zero padding after each NUL is not covered, and is therefore required
 * to BE zero (a header with junk after the terminator is malformed).
 * Fields are printable ASCII without spaces (0x21..0x7E); the version must
 * start with a numeric major.minor so an unparseable string can never be
 * ranked "equal to the floor" by the semver comparator.
 *
 * ── Protocol v1 header (BEGIN, command 0x01) — break-glass only ─────────────
 *
 *   132 bytes: image_size u32 LE | sha256[32] | signature[64] | version[32].
 *   The signature covers only (image_size_LE32 || sha256); product and
 *   version are outside it. A v1 BEGIN is refused unless the owner has armed
 *   break-glass (the BOOT-button provisioning gate, see ble_ota.h), and its
 *   acceptance is logged as a floor bypass.
 *
 * Decision table (decide() then apply_break_glass()):
 *
 *   key unprovisioned / policy unconfigured        → REFUSE
 *   malformed header                              → REFUSE
 *   v1, signature invalid                         → REFUSE (even when armed)
 *   v1, signature valid                           → NEEDS_BREAK_GLASS
 *   v2, signature invalid                         → REFUSE (even when armed)
 *   v2, product != running product                → REFUSE (even when armed)
 *   v2, version below max(running, NVS floor)     → NEEDS_BREAK_GLASS
 *   v2, version at or above that floor            → ACCEPT
 *   NEEDS_BREAK_GLASS + armed                     → ACCEPT_BREAK_GLASS
 *   NEEDS_BREAK_GLASS + not armed                 → REFUSE
 *
 * The floor is not raised by this module: like the pull path, the install
 * only records securacv_ota_mark_pending_install(); the new image raises
 * the floor to its own compiled version once its boot self-test confirms it
 * (securacv_ota_boot_self_test / securacv_ota_init). Raising it earlier
 * would, after a rollback, block ever re-offering that version.
 */

#ifndef SECURACV_BLE_OTA_POLICY_H
#define SECURACV_BLE_OTA_POLICY_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "securacv_ota.h"  // securacv_ota_update_decision + the v1 36-byte builder

namespace ble_ota {

// Control-characteristic command bytes.
enum Command : uint8_t {
  CMD_BEGIN_V1 = 0x01,  // legacy 132-byte OtaHeader follows (break-glass only)
  CMD_ABORT    = 0x02,  // no payload
  CMD_BEGIN_V2 = 0x03   // 168-byte OtaHeaderV2 follows
};

// Fixed field widths (including the NUL terminator).
static const size_t OTA_PRODUCT_WIDTH = 32;
static const size_t OTA_VERSION_WIDTH = 32;

// v1: 132-byte BEGIN payload. Packed so the wire layout is fixed regardless
// of compiler padding choices. Kept for the break-glass rescue path only.
struct __attribute__((packed)) OtaHeader {
  uint32_t image_size;        // total firmware bytes to expect
  uint8_t  sha256[32];        // SHA-256 of the firmware image
  uint8_t  signature[64];     // Ed25519 signature over (image_size_LE32 || sha256)
  char     version[32];       // null-terminated version string (UNSIGNED, informational)
};
static_assert(sizeof(OtaHeader) == 132, "OtaHeader (v1) wire size must be 132 bytes");

// v2: 168-byte BEGIN_V2 payload. Layout documented at the top of this file.
struct __attribute__((packed)) OtaHeaderV2 {
  uint8_t  magic[2];          // "SC"
  uint8_t  hdr_version;       // 0x02
  uint8_t  reserved;          // 0x00
  char     product[OTA_PRODUCT_WIDTH];  // NUL-terminated, zero-padded
  char     version[OTA_VERSION_WIDTH];  // NUL-terminated, zero-padded
  uint32_t image_size;        // little-endian
  uint8_t  sha256[32];
  uint8_t  signature[64];     // Ed25519 over build_v2_message()
};
static_assert(sizeof(OtaHeaderV2) == 168, "OtaHeaderV2 wire size must be 168 bytes");

static const uint8_t OTA_V2_MAGIC0      = 0x53;  // 'S'
static const uint8_t OTA_V2_MAGIC1      = 0x43;  // 'C'
static const uint8_t OTA_V2_HDR_VERSION = 0x02;

// Domain-separation prefix of the v2 canonical message (NUL-terminated on
// the wire, like every field that follows it).
static const char OTA_V2_DOMAIN[] = "scv-ble-ota-v2";

// Upper bound of build_v2_message() output:
//   14+1 (domain) + 31+1 (product) + 31+1 (version) + 10+1 (size) + 64+1 (sha)
//   = 155 bytes.
static const size_t OTA_V2_MSG_MAX = 160;

enum HeaderKind : uint8_t {
  HDR_NONE = 0,   // not parsed / malformed
  HDR_V1   = 1,
  HDR_V2   = 2
};

// What the policy acts on. Strings are NUL-terminated copies; a v1 header
// leaves product empty (it has none) and carries a sanitized, unsigned
// version string that is display data only.
struct ParsedHeader {
  HeaderKind kind;
  char       product[OTA_PRODUCT_WIDTH];
  char       version[OTA_VERSION_WIDTH];
  uint32_t   image_size;
  uint8_t    sha256[32];
  uint8_t    signature[64];
};

// Injected primitives + device facts — the only impure edges.
struct PolicyDeps {
  // Ed25519 verify: true iff sig(64) over msg by pub(32) is valid.
  bool (*ed25519_verify)(const uint8_t sig[64], const uint8_t pub[32],
                         const uint8_t* msg, size_t msg_len);
  const uint8_t* release_pubkey;   // 32 bytes; all-zero = unprovisioned → refuse
  const char*    product;          // running product id (OTA_PRODUCT); NULL → refuse
  const char*    running_version;  // FIRMWARE_VERSION; NULL → refuse
  const char*    nvs_floor;        // NVS anti-rollback floor, may be NULL / ""
};

enum Decision : uint8_t {
  DECISION_REFUSE             = 0,
  DECISION_ACCEPT             = 1,
  DECISION_NEEDS_BREAK_GLASS  = 2,  // valid signature, but v1 or below the floor
  DECISION_ACCEPT_BREAK_GLASS = 3   // NEEDS_BREAK_GLASS resolved by an armed owner
};

enum BreakGlassNeed : uint8_t {
  BG_NONE  = 0,
  BG_V1    = 1,   // legacy header: product and floor cannot be checked
  BG_FLOOR = 2    // v2 header whose version is below the floor
};

struct Verdict {
  Decision       decision;
  BreakGlassNeed need;
  const char*    reason;   // static string, < 64 bytes (fits ble_ota's last_error)
};

// Reason strings. Each is distinct so a client or a health-log reader can
// tell the refusals apart; every one fits ble_ota.cpp's 64-byte last_error.
static const char* const REASON_OK             = "ok";
static const char* const REASON_UNPROVISIONED  = "OTA disabled — release pubkey not provisioned";
static const char* const REASON_UNCONFIGURED   = "OTA policy not configured (product/version)";
static const char* const REASON_MALFORMED      = "header malformed";
static const char* const REASON_SIGNATURE      = "signature invalid";
static const char* const REASON_PRODUCT        = "product mismatch";
static const char* const REASON_V1_REFUSED     = "v1 header refused: protocol v2 required (not armed)";
static const char* const REASON_FLOOR_REFUSED  = "version below anti-rollback floor (not armed)";
static const char* const REASON_BREAK_GLASS_V1    = "break-glass: v1 header, product and floor unchecked";
static const char* const REASON_BREAK_GLASS_FLOOR = "break-glass: version below anti-rollback floor";

// ── internal helpers ───────────────────────────────────────────────────────

namespace policy_detail {

inline bool pubkey_provisioned(const uint8_t* pub) {
  if (!pub) return false;
  uint8_t acc = 0;
  for (size_t i = 0; i < 32; i++) acc |= pub[i];
  return acc != 0;
}

// A fixed-width string field is clean when it is non-empty, NUL-terminated
// inside its slot, printable ASCII without spaces before the NUL, and all
// zero after it (the padding is outside the signed message).
inline bool field_is_clean(const char* f, size_t width) {
  size_t n = 0;
  while (n < width && f[n] != '\0') {
    unsigned char c = (unsigned char)f[n];
    if (c < 0x21 || c > 0x7E) return false;
    n++;
  }
  if (n == 0 || n >= width) return false;   // empty, or no terminator
  for (size_t i = n; i < width; i++) if (f[i] != '\0') return false;
  return true;
}

// "MAJOR.MINOR" numeric prefix required — securacv_version_compare() ranks
// an unparseable string as EQUAL, which would read as "at the floor".
inline bool version_has_numeric_prefix(const char* v) {
  size_t i = 0;
  size_t digits = 0;
  while (v[i] >= '0' && v[i] <= '9') { i++; digits++; }
  if (digits == 0 || v[i] != '.') return false;
  i++;
  digits = 0;
  while (v[i] >= '0' && v[i] <= '9') { i++; digits++; }
  return digits > 0;
}

inline bool msg_append(uint8_t* out, size_t cap, size_t* pos, const char* field) {
  size_t len = strlen(field);
  if (*pos + len + 1 > cap) return false;
  memcpy(out + *pos, field, len);
  *pos += len;
  out[(*pos)++] = '\0';
  return true;
}

} // namespace policy_detail

// ── canonical message ──────────────────────────────────────────────────────

// Build the v2 canonical message (layout at the top of this file). Returns
// false — and writes nothing meaningful — if @p out is too small.
inline bool build_v2_message(const char* product, const char* version,
                             uint32_t image_size, const uint8_t sha256[32],
                             uint8_t* out, size_t cap, size_t* out_len) {
  if (!product || !version || !sha256 || !out || !out_len) return false;
  size_t pos = 0;
  if (!policy_detail::msg_append(out, cap, &pos, OTA_V2_DOMAIN)) return false;
  if (!policy_detail::msg_append(out, cap, &pos, product)) return false;
  if (!policy_detail::msg_append(out, cap, &pos, version)) return false;

  char size_dec[12];
  snprintf(size_dec, sizeof(size_dec), "%lu", (unsigned long)image_size);
  if (!policy_detail::msg_append(out, cap, &pos, size_dec)) return false;

  char sha_hex[65];
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    sha_hex[i * 2]     = k_hex[sha256[i] >> 4];
    sha_hex[i * 2 + 1] = k_hex[sha256[i] & 0x0F];
  }
  sha_hex[64] = '\0';
  if (!policy_detail::msg_append(out, cap, &pos, sha_hex)) return false;

  *out_len = pos;
  return true;
}

// ── parsing ────────────────────────────────────────────────────────────────

// Parse a v1 BEGIN payload (the bytes after the command byte). Tolerates
// trailing bytes, as the original protocol did. The version string is
// UNSIGNED: it is sanitized to printable ASCII for display and nothing else.
inline bool parse_v1(const uint8_t* buf, size_t len, ParsedHeader* out) {
  if (!buf || !out || len < sizeof(OtaHeader)) return false;
  OtaHeader hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  memset(out, 0, sizeof(*out));
  out->kind = HDR_V1;
  out->image_size = hdr.image_size;
  memcpy(out->sha256, hdr.sha256, 32);
  memcpy(out->signature, hdr.signature, 64);
  size_t o = 0;
  for (size_t i = 0; i < sizeof(hdr.version) && hdr.version[i] != '\0' &&
                     o < OTA_VERSION_WIDTH - 1; i++) {
    char c = hdr.version[i];
    // Same alphabet as v2's field_is_clean(): printable ASCII, no spaces.
    out->version[o++] = (c > 0x20 && c < 0x7f) ? c : '_';
  }
  out->version[o] = '\0';
  return true;
}

// Parse a v2 BEGIN_V2 payload (the bytes after the command byte). Strict:
// exact length, magic, header version, zero reserved byte, clean
// fixed-width fields, numeric version prefix. Any deviation is malformed.
inline bool parse_v2(const uint8_t* buf, size_t len, ParsedHeader* out) {
  if (!buf || !out || len != sizeof(OtaHeaderV2)) return false;
  OtaHeaderV2 hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.magic[0] != OTA_V2_MAGIC0 || hdr.magic[1] != OTA_V2_MAGIC1) return false;
  if (hdr.hdr_version != OTA_V2_HDR_VERSION) return false;
  if (hdr.reserved != 0) return false;
  if (!policy_detail::field_is_clean(hdr.product, OTA_PRODUCT_WIDTH)) return false;
  if (!policy_detail::field_is_clean(hdr.version, OTA_VERSION_WIDTH)) return false;
  if (!policy_detail::version_has_numeric_prefix(hdr.version)) return false;

  memset(out, 0, sizeof(*out));
  out->kind = HDR_V2;
  memcpy(out->product, hdr.product, OTA_PRODUCT_WIDTH);
  memcpy(out->version, hdr.version, OTA_VERSION_WIDTH);
  // image_size is little-endian on the wire; read it byte-wise so the
  // layout does not depend on host endianness.
  const uint8_t* p = buf + offsetof(OtaHeaderV2, image_size);
  out->image_size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  memcpy(out->sha256, hdr.sha256, 32);
  memcpy(out->signature, hdr.signature, 64);
  return true;
}

// ── decision ───────────────────────────────────────────────────────────────

namespace policy_detail {
inline Verdict make(Decision d, BreakGlassNeed n, const char* r) {
  Verdict v;
  v.decision = d;
  v.need     = n;
  v.reason   = r;
  return v;
}
} // namespace policy_detail

// Decide whether a parsed BEGIN header may open a session. Signature checks
// run BEFORE anything else about the header is believed. Never returns
// ACCEPT_BREAK_GLASS itself — that resolution is apply_break_glass()'s,
// so the caller decides when to consume the owner's arming.
inline Verdict decide(const ParsedHeader& h, const PolicyDeps& d) {
  using policy_detail::make;

  if (!d.ed25519_verify || !policy_detail::pubkey_provisioned(d.release_pubkey))
    return make(DECISION_REFUSE, BG_NONE, REASON_UNPROVISIONED);
  if (!d.product || d.product[0] == '\0' ||
      !d.running_version || d.running_version[0] == '\0')
    return make(DECISION_REFUSE, BG_NONE, REASON_UNCONFIGURED);

  if (h.kind == HDR_V1) {
    // Legacy header: the release key still has to have signed the image
    // (size || sha256) — break-glass relaxes the floor, never the signature.
    uint8_t msg[36];
    securacv_ota_build_signed_message(h.image_size, h.sha256, msg);
    if (!d.ed25519_verify(h.signature, d.release_pubkey, msg, sizeof(msg)))
      return make(DECISION_REFUSE, BG_NONE, REASON_SIGNATURE);
    return make(DECISION_NEEDS_BREAK_GLASS, BG_V1, REASON_BREAK_GLASS_V1);
  }

  if (h.kind == HDR_V2) {
    uint8_t msg[OTA_V2_MSG_MAX];
    size_t msg_len = 0;
    if (!build_v2_message(h.product, h.version, h.image_size, h.sha256,
                          msg, sizeof(msg), &msg_len))
      return make(DECISION_REFUSE, BG_NONE, REASON_MALFORMED);
    if (!d.ed25519_verify(h.signature, d.release_pubkey, msg, msg_len))
      return make(DECISION_REFUSE, BG_NONE, REASON_SIGNATURE);

    // Product binding is never bypassed: a signed image for another
    // product is a brick, not a rescue.
    if (strcmp(h.product, d.product) != 0)
      return make(DECISION_REFUSE, BG_NONE, REASON_PRODUCT);

    // Same floor as the pull path: max(running version, NVS floor).
    // Equal is accepted — re-flashing the running version is a repair
    // and cannot move the floor.
    securacv_ota_decision_t vd = securacv_ota_update_decision(
        h.version, d.running_version, d.nvs_floor);
    if (vd == SECURACV_OTA_DECISION_ROLLBACK)
      return make(DECISION_NEEDS_BREAK_GLASS, BG_FLOOR, REASON_BREAK_GLASS_FLOOR);
    return make(DECISION_ACCEPT, BG_NONE, REASON_OK);
  }

  return make(DECISION_REFUSE, BG_NONE, REASON_MALFORMED);
}

// Resolve a NEEDS_BREAK_GLASS verdict against whether the owner has armed
// break-glass. Any other verdict passes through unchanged.
inline Verdict apply_break_glass(const Verdict& v, bool armed) {
  if (v.decision != DECISION_NEEDS_BREAK_GLASS) return v;
  if (armed) return policy_detail::make(DECISION_ACCEPT_BREAK_GLASS, v.need, v.reason);
  return policy_detail::make(DECISION_REFUSE, v.need,
                             v.need == BG_V1 ? REASON_V1_REFUSED : REASON_FLOOR_REFUSED);
}

} // namespace ble_ota

#endif // SECURACV_BLE_OTA_POLICY_H
