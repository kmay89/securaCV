#pragma once
//
// device_pseudonym — stable, non-reversible device identifier for operator-facing
// diagnostics (HTTP APIs, serial logs).
//
// Privacy (Invariant III / event_contract §10): the device MUST NOT expose its raw
// hardware MAC address in API payloads or logs. This derives a salted pseudonymous
// token from the efuse MAC and a per-device NVS secret, so diagnostics can still show
// a stable handle without leaking a network-trackable identifier.
//
#include <stddef.h>
#include <stdint.h>

namespace device_pseudonym {

constexpr size_t SECRET_LEN   = 32;             // per-device salt length
constexpr size_t TOKEN_BYTES  = 8;              // 64-bit pseudonym
constexpr size_t HEX_LEN      = TOKEN_BYTES * 2; // 16 hex chars (excl. NUL)
constexpr char   NVS_SALT_KEY[] = "id_salt";    // NVS blob key for the salt

// Pure, non-reversible derivation:
//   token = SHA256("canary:device-id:v1:" || secret || mac)[0..TOKEN_BYTES]  (lowercase hex)
// Stable for a fixed (secret, mac); the raw mac cannot be recovered from the output.
// Writes HEX_LEN+1 bytes (incl. NUL) into out_hex. Returns false on bad args/buffer.
// Host-testable: the SHA-256 backend is selected at compile time (mbedtls on device,
// OpenSSL off-device) so this exact construction can be exercised without hardware.
bool derive(const uint8_t* mac, size_t mac_len,
            const uint8_t* secret, size_t secret_len,
            char* out_hex, size_t out_len);

#if defined(ARDUINO)
// Device-side convenience: lazily loads (or creates) the per-device salt in NVS, reads
// the efuse MAC, and writes the pseudonym hex. Never exposes the raw MAC.
bool device_id_hex(char* out_hex, size_t out_len);
#endif

} // namespace device_pseudonym
