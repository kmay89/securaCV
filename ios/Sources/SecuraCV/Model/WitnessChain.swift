// WitnessChain.swift
//
// The tamper-evident, Ed25519-signed hash chain a Canary exposes at
// GET /api/v1/witness. This is the durable record — events as *claims*, never
// pixels. The phone can verify the whole chain on-device (CryptoKit), so trust
// never depends on us.

import Foundation

/// One record from the witness chain (see canary-vision/docs/api.md).
/// hash = SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}")
struct WitnessRecord: Identifiable, Codable, Hashable, Sendable {
    var seq: UInt64
    var hash: String
    var prevHash: String
    var timestamp: Date
    var eventType: String
    var zone: String
    var signature: String

    var id: UInt64 { seq }

    enum CodingKeys: String, CodingKey {
        case seq, hash
        case prevHash = "prev_hash"
        case timestamp
        case eventType = "event_type"
        case zone, signature
    }

    /// The exact preimage the firmware hashes — recomputed here to check the
    /// chain link locally. Kept in one place so it can't drift from the spec.
    var hashPreimage: String {
        let iso = ISO8601DateFormatter.witness.string(from: timestamp)
        return "\(seq):\(prevHash):\(iso):\(eventType):\(zone)"
    }

    /// Human severity for a raw event_type — resolved by the one shared
    /// vocabulary (Shared/EventVocabulary.swift), which understands the
    /// dictionary ids, the device dialect, and calmly defaults everything
    /// else to `.notice`. Same coarse meanings as const.py, one copy.
    var severity: Severity { EventVocabulary.severity(forWire: eventType) }
}

struct WitnessChainPage: Codable, Sendable {
    var records: [WitnessRecord]
}

/// A single line in the Today timeline — an event promoted to something a human
/// reads, carrying its trust badge. Never a clip, never a face; a claim.
struct TimelineEvent: Identifiable, Hashable, Sendable {
    var id: String                       // "\(deviceID)#\(seq)"
    var deviceID: String
    var deviceName: String
    var zone: String
    var headline: String                 // "Package at the front door"
    var severity: Severity
    var badge: TrustBadge
    /// Coarse 10-minute bucket, honoring Invariant III — never a precise second.
    var timeBucket: Date
    /// SF Symbol for the event's meaning (EventVocabulary) — defaulted so
    /// hand-built events (demo, tests) stay valid without naming one.
    var symbol: String = "sparkle"
}

extension ISO8601DateFormatter {
    static let witness: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()
}
