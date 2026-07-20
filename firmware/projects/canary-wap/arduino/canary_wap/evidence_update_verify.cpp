/*
 * SecuraCV Canary — drop-file update verification. Pure; see header. The
 * check order mirrors securacv_ota.cpp's network path exactly — if you
 * change one, change both, and say why in docs/firmware_ota.md.
 */

#include "evidence_update_verify.h"

#include <string.h>
#include <stdio.h>

namespace evidence_update {

static Verdict fail(const char* code, const char* msg) {
  Verdict v;
  v.ok = false;
  v.code = code;
  snprintf(v.msg, sizeof v.msg, "%s", msg);
  return v;
}

static bool pubkey_provisioned(const uint8_t* key) {
  if (!key) return false;
  uint8_t acc = 0;
  for (int i = 0; i < 32; i++) acc |= key[i];
  return acc != 0;
}

Verdict verify(const securacv_ota_manifest_t& m,
               const uint8_t* image, uint32_t image_len,
               const VerifyDeps& d) {
  // 0) Fail closed without a provisioned release key — same rule as the
  //    network path's SECURACV_OTA_ERR_PUBKEY_MISSING.
  if (!pubkey_provisioned(d.release_pubkey))
    return fail("pubkey_missing", "this build carries no release key - updates are disabled");

  // 1) Product identity: an image for another Canary never installs here.
  if (!d.product || strcmp(m.product, d.product) != 0)
    return fail("product_mismatch", "that update is for a different SecuraCV product");

  // 2) Manifest metadata signature (Ed25519 over the canonical field string).
  if (m.manifest_signature[0] == '\0')
    return fail("manifest_sig_missing", "the manifest carries no signature - re-download it from the release");
  {
    uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
    size_t msg_len = 0;
    if (!securacv_ota_build_manifest_message(&m, msg, sizeof msg, &msg_len))
      return fail("manifest_msg", "the manifest could not be canonicalized");
    uint8_t sig[64];
    if (!securacv_ota_hex_to_bytes(m.manifest_signature, sig, sizeof sig))
      return fail("manifest_sig_format", "the manifest signature is malformed");
    if (!d.ed25519_verify(sig, d.release_pubkey, msg, msg_len))
      return fail("manifest_sig_invalid", "the manifest signature does not verify - not a SecuraCV release");
  }

  // 3) Size: the image signature covers it, so it must match exactly.
  if (image_len == 0 || m.size != image_len)
    return fail("size_mismatch", "the image size does not match the manifest - copy both files again");

  // 4) SHA-256 of the staged bytes vs the manifest.
  uint8_t sha[32];
  d.sha256(image, image_len, sha);
  {
    uint8_t want[32];
    if (!securacv_ota_hex_to_bytes(m.sha256, want, sizeof want))
      return fail("sha_format", "the manifest sha256 is malformed");
    if (memcmp(sha, want, 32) != 0)
      return fail("sha_mismatch", "the image does not match its manifest - copy both files again");
  }

  // 5) Image signature over size_LE32 || sha256.
  {
    uint8_t signed_msg[36];
    securacv_ota_build_signed_message(image_len, sha, signed_msg);
    uint8_t sig[64];
    if (m.signature[0] == '\0' ||
        !securacv_ota_hex_to_bytes(m.signature, sig, sizeof sig))
      return fail("image_sig_format", "the image signature is missing or malformed");
    if (!d.ed25519_verify(sig, d.release_pubkey, signed_msg, sizeof signed_msg))
      return fail("image_sig_invalid", "the image signature does not verify - not a SecuraCV release");
  }

  // 6) Version policy: identical to the network path — only a genuine
  //    upgrade installs; same-version and rollback are refused (the
  //    anti-rollback floor is monotonic and NVS-persisted).
  switch (securacv_ota_update_decision(m.version, d.running_version, d.nvs_floor)) {
    case SECURACV_OTA_DECISION_UPDATE:
      break;
    case SECURACV_OTA_DECISION_UP_TO_DATE:
      return fail("up_to_date", "that version is already running");
    default:
      return fail("rollback", "that version is older than this board allows (anti-rollback)");
  }

  Verdict v;
  v.ok = true;
  v.code = "ok";
  snprintf(v.msg, sizeof v.msg, "verified %s %s - installing on reboot", m.product, m.version);
  return v;
}

} // namespace evidence_update
