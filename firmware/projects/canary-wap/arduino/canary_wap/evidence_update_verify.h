/*
 * SecuraCV Canary — drop-file update verification (host-testable core).
 *
 * The USB update drop-zone (docs/design/usb_evidence_drive.md) is a courier,
 * not an authority: a staged image installs only if it passes the exact
 * checks the network OTA path applies, in the same order. This module IS
 * that pipeline, factored pure: it calls the engine's own canonical-message
 * builders (securacv_ota_build_manifest_message / _build_signed_message /
 * securacv_ota_update_decision) and takes the crypto primitives as injected
 * function pointers so the firmware passes Crypto/mbedtls and the host test
 * passes OpenSSL — same bytes either way.
 *
 * No Arduino/ESP includes. Compiled on device by usb_evidence_drive.cpp and
 * on the host by tests_host/test_evidence_update_verify.cpp (which signs
 * with real Ed25519 keys and proves accept/reject behavior end-to-end).
 */

#ifndef SECURACV_EVIDENCE_UPDATE_VERIFY_H
#define SECURACV_EVIDENCE_UPDATE_VERIFY_H

#include <stdint.h>
#include <stddef.h>

#include "securacv_ota.h" // securacv_ota_manifest_t + pure builders (host-safe)

namespace evidence_update {

// Injected primitives — the only impure edges of the pipeline.
struct VerifyDeps {
  // Ed25519 verify: true iff sig(64) over msg by pub(32) is valid.
  bool (*ed25519_verify)(const uint8_t sig[64], const uint8_t pub[32],
                         const uint8_t* msg, size_t msg_len);
  // SHA-256 of a memory range.
  void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
  const uint8_t* release_pubkey;  // 32 bytes; all-zero = unprovisioned → refuse
  const char* product;            // e.g. "securacv-canary-wap"
  const char* running_version;    // FIRMWARE_VERSION
  const char* nvs_floor;          // anti-rollback floor, may be NULL/""
};

struct Verdict {
  bool ok = false;
  const char* code = "";   // stable machine-ish tag for logs/tests
  char msg[112] = {0};     // one human sentence → RESULT.TXT
};

// Run the full acceptance pipeline against a staged manifest + image:
//   pubkey provisioned → product match → manifest signature → size match →
//   SHA-256 of the staged bytes → image signature over size||sha →
//   version decision (must be UPDATE: same-version and rollback refused,
//   identical to the network path).
Verdict verify(const securacv_ota_manifest_t& m,
               const uint8_t* image, uint32_t image_len,
               const VerifyDeps& d);

} // namespace evidence_update

#endif // SECURACV_EVIDENCE_UPDATE_VERIFY_H
