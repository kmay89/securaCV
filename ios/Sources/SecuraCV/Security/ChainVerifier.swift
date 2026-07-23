// ChainVerifier.swift
//
// Verify a Canary's witness chain entirely on the phone: the hash links line
// up, and the head is Ed25519-signed by the TOFU-pinned key. "You shouldn't
// have to believe us" — this is that promise, in ~80 lines, no network, no
// vendor. Pure and host-testable (it's in the SecuraCVTests target).

import Foundation
import CryptoKit

enum ChainVerdict: Equatable {
    case verified                 // links intact AND signature checks out
    case signedUnpinned           // signature present, no pinned key yet (TOFU first sight)
    case unsigned                 // no signature on the head
    case brokenLink(seq: UInt64)  // a prev_hash / hash mismatch — tamper
    case signatureFailed          // head signature did not verify — loud

    var badge: TrustBadge {
        switch self {
        case .verified: return .verified
        case .signedUnpinned: return .signed
        case .unsigned: return .unsigned
        case .brokenLink, .signatureFailed: return .failed
        }
    }
}

struct ChainVerifier {

    /// Recompute every hash and confirm each record chains to its predecessor.
    /// Returns the first break, or nil if the whole run is internally consistent.
    static func firstBrokenLink(in records: [WitnessRecord]) -> UInt64? {
        let ordered = records.sorted { $0.seq < $1.seq }
        var expectedPrev: String? = nil
        for r in ordered {
            let recomputed = sha256Hex(r.hashPreimage)
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
        guard let head = page.records.max(by: { $0.seq < $1.seq }) else { return .unsigned }

        if let broken = firstBrokenLink(in: page.records) {
            return .brokenLink(seq: broken)
        }
        guard !head.signature.isEmpty, let sig = Data(hexString: head.signature) else {
            return .unsigned
        }
        guard let keyData = pinnedKey else {
            return .signedUnpinned
        }
        guard let key = try? Curve25519.Signing.PublicKey(rawRepresentation: keyData) else {
            return .signatureFailed
        }
        let message = Data(head.hash.utf8)     // the head hash is what the device signs
        return key.isValidSignature(sig, for: message) ? .verified : .signatureFailed
    }

    static func sha256Hex(_ s: String) -> String {
        SHA256.hash(data: Data(s.utf8)).map { String(format: "%02x", $0) }.joined()
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
