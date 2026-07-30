// DeviceFingerprint.swift
//
// The third canonical contract the app has to speak: how a Canary's 8-byte
// public-key fingerprint is derived. Ported from `compute_fingerprint()` in
// `firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino`:
//
//     sha256_domain("securacv:pubkey:fingerprint", pubkey[32]) -> hash[32]
//     fp = hash[0..<8]
//
// and `sha256_domain` is a domain-separated SHA-256 over
//
//     <domain-string> || 0x00 || <data>
//
// The last two bytes of that fingerprint are exactly what rides the BLE
// presence beacon (`s_beacon_fp = dev.pubkey_fp[6..7]`), which is what lets the
// app tie a heard beacon to a Canary it has already pinned a key for.
//
// WHY THIS EXISTS: without it, `Witness.fingerprint` is empty for every paired
// device, `FleetMerge.attach` can never match, and each paired Canary shows up
// TWICE — once as its real row (possibly `.lost`, raising a false alert) and
// once as an anonymous "SCV-XXXX" row. Deriving the fingerprint from the key we
// already pinned closes that loop without a new endpoint or a firmware change.
//
// PURE: Foundation + CryptoKit, no network, no transport. Host-testable, and
// `DeviceFingerprintTests` pins it to an independently computed vector.

import Foundation
import CryptoKit

enum DeviceFingerprint {
    /// The firmware's domain-separation string. Changing it changes every
    /// device's identity — it is a wire constant, not a tunable.
    static let domain = "securacv:pubkey:fingerprint"
    /// `memcpy(fp, hash, 8)` — the fingerprint is the first 8 bytes.
    static let byteCount = 8

    /// Raw 8-byte fingerprint of an Ed25519 public key.
    /// Returns nil for a key that isn't 32 bytes, rather than fingerprinting
    /// something that cannot be a public key.
    static func bytes(forPublicKey pub: Data) -> Data? {
        guard pub.count == 32 else { return nil }
        var hasher = SHA256()
        hasher.update(data: Data(domain.utf8))
        hasher.update(data: Data([0x00]))          // the domain separator
        hasher.update(data: pub)
        return Data(hasher.finalize().prefix(byteCount))
    }

    /// The 16-hex form the app stores on `Witness.fingerprint`, matching the
    /// firmware's `%02x`-per-byte rendering.
    static func hex(forPublicKey pub: Data) -> String? {
        guard let fp = bytes(forPublicKey: pub) else { return nil }
        return fp.map { String(format: "%02x", $0) }.joined()
    }
}
