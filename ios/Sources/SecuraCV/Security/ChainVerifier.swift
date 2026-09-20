// ChainVerifier.swift
//
// Verify a Canary's witness chain entirely on the phone: the hash links line
// up, and the head is Ed25519-signed by the TOFU-pinned key. "You shouldn't
// have to believe us" — this is that promise, in ~100 lines, no network, no
// vendor. Pure and host-testable (it's in the SecuraCVTests target).
//
// Two chain formats, one verdict (spec/witness_api_v1.md §4): the reference
// device-api hashes a seven-field string and signs the hex hash's UTF-8;
// canary-wap hashes a domain-separated binary pre-image and signs the raw
// 32-byte hash. The page says which (`chain_format`); the verifier
// recomputes with that construction and refuses to guess at one it does not
// know. Both formats share the trust rule: every link recomputed, the head's
// signature checked against the pinned key, and "verified" means exactly
// that check (AGENTS.md rule 4).

import Foundation
import CryptoKit

enum ChainVerdict: Equatable {
    case verified                 // links intact AND signature checks out
    case signedUnpinned           // signature present, no pinned key yet (TOFU first sight)
    case unsigned                 // no signature on the head
    case brokenLink(seq: UInt64)  // a prev_hash / hash mismatch — tamper
    case signatureFailed          // head signature did not verify — loud
    /// The page names a chain_format this build cannot recompute. Nothing
    /// stronger than "unverified" is honest — and nothing weaker: it is not
    /// a tamper finding either.
    case unsupportedFormat(String)

    var badge: TrustBadge {
        switch self {
        case .verified: return .verified
        case .signedUnpinned: return .signed
        case .unsigned: return .unsigned
        case .brokenLink, .signatureFailed: return .failed
        case .unsupportedFormat: return .unknown
        }
    }
}

struct ChainVerifier {

    /// Recompute every hash and confirm each record chains to its predecessor.
    /// Returns the first break, or nil if the whole run is internally consistent.
    /// A wap_v1 record that lacks a pre-image field cannot be recomputed and
    /// is reported as the break — the seq-binding rule: the signature covers
    /// only the hash, so an unrecomputed hash proves nothing about seq.
    static func firstBrokenLink(in records: [WitnessRecord],
                                format: WitnessChainFormat = .referenceV1) -> UInt64? {
        let ordered = records.sorted { $0.seq < $1.seq }
        var expectedPrev: String? = nil
        for r in ordered {
            guard let recomputed = recomputedHash(of: r, format: format) else { return r.seq }
            if recomputed.caseInsensitiveCompare(r.hash) != .orderedSame {
                return r.seq
            }
            if let prev = expectedPrev, prev.caseInsensitiveCompare(r.prevHash) != .orderedSame {
                return r.seq
            }
            expectedPrev = r.hash
        }
        return nil
    }

    /// Full verdict for a chain page against an optional pinned public key.
    /// - pinnedKey: the raw 32-byte Ed25519 public key pinned on first use for
    ///   this device (nil the very first time we see it — that's TOFU).
    static func verify(_ page: WitnessChainPage, pinnedKey: Data?) -> ChainVerdict {
        guard let format = page.format else {
            return .unsupportedFormat(page.chainFormat ?? "")
        }
        guard let head = page.records.max(by: { $0.seq < $1.seq }) else { return .unsigned }

        if let broken = firstBrokenLink(in: page.records, format: format) {
            return .brokenLink(seq: broken)
        }
        // An absent or empty signature is "not individually signed" — the
        // contract allows it, and the honest badge is Unsigned, never
        // Verified, whatever key we hold.
        guard !head.signature.isEmpty, let sig = Data(hexString: head.signature) else {
            return .unsigned
        }
        guard let keyData = pinnedKey else {
            return .signedUnpinned
        }
        guard let key = try? Curve25519.Signing.PublicKey(rawRepresentation: keyData) else {
            return .signatureFailed
        }
        guard let message = signedMessage(for: head, format: format) else {
            return .brokenLink(seq: head.seq)   // an unparsable head hash never verifies
        }
        return key.isValidSignature(sig, for: message) ? .verified : .signatureFailed
    }

    /// What the device's Ed25519 signature covers, per format (spec §4).
    static func signedMessage(for record: WitnessRecord, format: WitnessChainFormat) -> Data? {
        switch format {
        case .referenceV1:
            return Data(record.hash.utf8)          // the hex STRING's UTF-8 bytes
        case .wapV1:
            guard let raw = Data(hexString: record.hash), raw.count == 32 else { return nil }
            return raw                             // the RAW 32-byte chain hash
        }
    }

    /// The record's hash recomputed from its own fields, hex; nil when a
    /// pre-image field the format needs is missing or malformed.
    static func recomputedHash(of r: WitnessRecord, format: WitnessChainFormat) -> String? {
        switch format {
        case .referenceV1:
            return sha256Hex(r.hashPreimage)
        case .wapV1:
            guard let prev = Data(hexString: r.prevHash), prev.count == 32,
                  let payloadHex = r.payloadHash,
                  let payload = Data(hexString: payloadHex), payload.count == 32,
                  let bucket = r.timeBucket,
                  r.seq <= UInt64(UInt32.max) else { return nil }
            return hex(wapChainHash(prevHash: prev, payloadHash: payload,
                                    seq: UInt32(r.seq), timeBucket: bucket))
        }
    }

    /// canary_wap.ino compute_chain_hash, byte for byte:
    /// SHA-256("securacv:fw:chain:v1" ‖ 0x00 ‖ prev ‖ payload_hash ‖ seq(BE32) ‖ time_bucket(BE32)).
    static func wapChainHash(prevHash: Data, payloadHash: Data, seq: UInt32, timeBucket: UInt32) -> Data {
        var message = Data("securacv:fw:chain:v1".utf8)
        message.append(0x00)
        message.append(prevHash)
        message.append(payloadHash)
        message.append(contentsOf: bigEndianBytes(seq))
        message.append(contentsOf: bigEndianBytes(timeBucket))
        return Data(SHA256.hash(data: message))
    }

    /// The wap_v1 genesis: SHA-256("securacv:genesis:v1" ‖ 0x00 ‖ device_id).
    /// Optional to check (a `last=N` page rarely reaches seq 1); here so a
    /// test can build a chain from the same root the firmware does.
    static func wapGenesis(deviceID: String) -> Data {
        var message = Data("securacv:genesis:v1".utf8)
        message.append(0x00)
        message.append(Data(deviceID.utf8))
        return Data(SHA256.hash(data: message))
    }

    private static func bigEndianBytes(_ v: UInt32) -> [UInt8] {
        [UInt8(truncatingIfNeeded: v >> 24), UInt8(truncatingIfNeeded: v >> 16),
         UInt8(truncatingIfNeeded: v >> 8), UInt8(truncatingIfNeeded: v)]
    }

    static func sha256Hex(_ s: String) -> String {
        hex(Data(SHA256.hash(data: Data(s.utf8))))
    }

    static func hex(_ data: Data) -> String {
        data.map { String(format: "%02x", $0) }.joined()
    }
}

extension Data {
    /// Parse a hex string ("a1b2…") into bytes; nil on odd length / bad chars.
    init?(hexString: String) {
        let chars = Array(hexString)
        guard chars.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = chars[i].hexDigitValue, let lo = chars[i + 1].hexDigitValue else { return nil }
            bytes.append(UInt8(hi << 4 | lo))
            i += 2
        }
        self = Data(bytes)
    }
}
