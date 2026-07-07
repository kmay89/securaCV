#pragma once
#include <stddef.h>
#include <stdint.h>

// Witness identity + tamper-evident chain for the optical witness.
// Deliberately IDENTICAL to canary-sense/include/canary/witness.h —
// same NVS contract, same shared signer, same canonical chain core.
//
// Identity: an Ed25519 keypair generated from the hardware RNG on first
// boot and persisted in NVS (namespace "securacv", key "privkey") — the
// same storage contract as the canary-wap tree, so the keypair survives
// reflashes and OTA installs. The fingerprint is the wap formula:
// SHA256("securacv:pubkey:fingerprint" || 0x00 || pubkey)[0..8] hex.
//
// Signing: every published event carries an Ed25519 signature over the
// v1 `sense` canonical message (see common/identity/device_signature.h);
// Home Assistant rebuilds the canonical from the JSON fields and
// verifies against the TOFU-pinned pubkey (published on the health
// topic). The chain publish reuses the generic v1 `chain` canonical, so
// HA's existing verify_chain works unchanged.
//
// Chain: the CANONICAL construction from common/witness/witness_chain.h
// (one definition across every firmware; off-device verifier:
// tools/verify_witness_log.py):
//   payload_hash = SHA256("securacv:fw:payload:v1" || 0x00 || canonical)
//   head'        = SHA256("securacv:fw:chain:v1"   || 0x00 ||
//                         head || payload_hash || seq_BE || bucket_BE)
//   head0        = SHA256("securacv:genesis:v1"    || 0x00 || device_id)
// seq and the coarse uptime bucket are IN the hash, so records cannot be
// silently renumbered or time-shifted. Devices upgraded from the older
// pubkey-bound genesis keep their stored NVS head and continue forward.
// Head + length persist in NVS on every advance (presence transitions
// are rare, so flash wear is negligible).

namespace canary::witness {

// Load-or-generate the keypair, derive the fingerprint, initialize the
// shared device_signature module, and load (or start) the chain. Call
// once in setup() after canary::cfg::get() is available. Returns false
// if key material could not be established (signing disabled; the
// witness still publishes, unsigned).
bool init();
bool ready();

// Build the signature envelope for one event — the JSON fragment
//   ,"v":1,"alg":"ed25519","fp":"<16hex>","sig":"<86 b64url>"
// (leading comma included) written into out. On any failure out is set
// to the empty string and false is returned: the caller publishes the
// event unsigned rather than half-signed.
bool sign_event_envelope(uint32_t    seq,
                         const char* event_name,
                         const char* presence,
                         const char* occupants,
                         const char* range,
                         uint32_t    bucket_uptime_s,
                         char*       out,
                         size_t      cap);

// Advance the hash chain over the same canonical bytes that were signed,
// then persist head + length to NVS.
void chain_advance(uint32_t    seq,
                   const char* event_name,
                   const char* presence,
                   const char* occupants,
                   const char* range,
                   uint32_t    bucket_uptime_s);

uint32_t       chain_length();
const uint8_t* chain_head();       // 32 bytes, static storage

} // namespace canary::witness
