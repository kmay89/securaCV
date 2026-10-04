#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_model.h"

// TOFU trust store + on-device Ed25519 chain verification.
//
// The display holds the same bar as the Home Assistant component
// (custom_components/securacv/device_trust.py): the FIRST pubkey a device
// publishes on its retained health topic is pinned (trust-on-first-use,
// persisted to NVS so the pin survives reboots and OTA), and every signed
// chain head is verified against that pin — locally, on the display's own
// silicon. "Verified ✓" on the glass therefore means exactly what it means
// in HA: an Ed25519 signature over the locked canonical
//
//   securacv-canary-sig|v1|chain|<device_id>|<length>|<latest_hash_hex>
//
// checked out against the key this household pinned. A later, DIFFERENT
// pubkey for a pinned device is a mismatch — surfaced, never silently
// re-pinned.

namespace canary::trust {

// Load pins from NVS. Call once in setup() before any evaluate.
void init();

// TOFU: pin `pubkey_hex` (64 hex chars) for device_id on first sight.
// Returns false on a mismatch with an existing pin (caller should treat the
// device's trust surface as Failed until the operator intervenes).
bool note_pubkey(const char* device_id, const char* pubkey_hex);

// Verdict for a retained chain payload. sig_b64url/fp may be null/empty
// (older unsigned publishers) — that degrades to Unsigned, not Failed.
canary::fleet::Badge evaluate_chain(const char* device_id,
                                    uint32_t length,
                                    const char* latest_hash_hex,
                                    const char* sig_b64url);

// Number of pinned devices (diagnostics).
int pinned_count();

// Proof-on-Glass: the pinned pubkey (64 hex + NUL) for a device, so the
// proof QR can carry the key the verifier checks against. False = no pin.
bool pinned_pubkey_hex(const char* device_id, char out[65]);

}  // namespace canary::trust
