// WitnessChain.swift
//
// The tamper-evident, Ed25519-signed hash chain a Canary exposes at
// GET /api/v1/witness. This is the durable record — events as *claims*, never
// pixels. The phone can verify the whole chain on-device (CryptoKit), so trust
// never depends on us.

import Foundation

/// One record from the witness chain (see canary-vision/docs/api.md).
/// hash = SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}")
/// — seven fields. time_source defaults to "device_clock"; gps_timestamp is
/// absent on the wire without a GPS fix and hashes as the empty string.
struct WitnessRecord: Identifiable, Codable, Hashable, Sendable {
    var seq: UInt64
    var hash: String
    var prevHash: String
    var timestamp: Date
    var eventType: String
    var zone: String
    var signature: String
    /// "device_clock" or "gps_utc" — part of the signed hash preimage.
    var timeSource: String = "device_clock"
    /// The GPS-derived UTC string when time_source is gps_utc; empty (and
    /// absent on the wire) otherwise. Part of the signed hash preimage.
    var gpsTimestamp: String = ""

    var id: UInt64 { seq }

    enum CodingKeys: String, CodingKey {
        case seq, hash
        case prevHash = "prev_hash"
        case timestamp
        case eventType = "event_type"
        case zone, signature
        case timeSource = "time_source"
        case gpsTimestamp = "gps_timestamp"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        seq = try c.decode(UInt64.self, forKey: .seq)
        hash = try c.decode(String.self, forKey: .hash)
        prevHash = try c.decode(String.self, forKey: .prevHash)
        timestamp = try c.decode(Date.self, forKey: .timestamp)
        eventType = try c.decode(String.self, forKey: .eventType)
        zone = try c.decode(String.self, forKey: .zone)
        signature = try c.decode(String.self, forKey: .signature)
        // Wire defaults match device-state.js: time_source is always written
        // but tolerate its absence; gps_timestamp is omitted without a fix.
        timeSource = try c.decodeIfPresent(String.self, forKey: .timeSource) ?? "device_clock"
        gpsTimestamp = try c.decodeIfPresent(String.self, forKey: .gpsTimestamp) ?? ""
    }

    init(seq: UInt64, hash: String, prevHash: String, timestamp: Date,
         eventType: String, zone: String, signature: String,
         timeSource: String = "device_clock", gpsTimestamp: String = "") {
        self.seq = seq
        self.hash = hash
        self.prevHash = prevHash
        self.timestamp = timestamp
        self.eventType = eventType
        self.zone = zone
        self.signature = signature
        self.timeSource = timeSource
        self.gpsTimestamp = gpsTimestamp
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(seq, forKey: .seq)
        try c.encode(hash, forKey: .hash)
        try c.encode(prevHash, forKey: .prevHash)
        try c.encode(timestamp, forKey: .timestamp)
        try c.encode(eventType, forKey: .eventType)
        try c.encode(zone, forKey: .zone)
        try c.encode(signature, forKey: .signature)
        try c.encode(timeSource, forKey: .timeSource)
        if !gpsTimestamp.isEmpty { try c.encode(gpsTimestamp, forKey: .gpsTimestamp) }
    }

    /// The exact preimage the firmware hashes — recomputed here to check the
    /// chain link locally. Kept in one place so it can't drift from the spec.
    /// Pinned against vectors produced by the device's own hashing code in
    /// WitnessChainPreimageTests — this string USED to omit the two trailing
    /// fields, which made every healthy device verify as tampered.
    var hashPreimage: String {
        let iso = ISO8601DateFormatter.witness.string(from: timestamp)
        return "\(seq):\(prevHash):\(iso):\(eventType):\(zone):\(timeSource):\(gpsTimestamp)"
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
