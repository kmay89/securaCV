// WitnessChain.swift
//
// The tamper-evident, Ed25519-signed hash chain a Canary exposes at
// GET /api/v1/witness — the ONE page contract (spec/witness_api_v1.md),
// served by the canary-vision reference device-api (`chain_format`
// reference_v1) and by canary-wap firmware (`wap_v1`). This is the durable
// record — events as *claims*, never pixels. The phone can verify the whole
// chain on-device (CryptoKit), so trust never depends on us.
//
// Decoding is tolerant the way every wire struct here is: the contract's
// required fields must be present, everything else defaults. Two tolerances
// are load-bearing for trust, so they are spelled out:
//   * `signature` absent or empty decodes as "" and means NOT individually
//     signed — ChainVerifier can never call such a head verified;
//   * `timestamp` absent (a canary-wap that has not met a clock) is anchored
//     locally from `time_bucket × time_bucket_ms` against the page's
//     `uptime_s`, and stays coarse — never a 1970 date, never a second.

import Foundation

/// Which chain construction a page's records use (spec/witness_api_v1.md §4).
enum WitnessChainFormat: String, Sendable {
    /// canary-vision device-api: seven-field string pre-image; the signature
    /// covers the UTF-8 of the hex hash.
    case referenceV1 = "reference_v1"
    /// canary-wap firmware: domain-separated binary pre-image over
    /// prev ‖ payload_hash ‖ seq ‖ time_bucket; the signature covers the RAW
    /// 32-byte hash.
    case wapV1 = "wap_v1"

    /// An absent or empty `chain_format` is the reference server, which
    /// predates the field. Nil means a format this build does not know —
    /// the honest verdict is then "unverified", never a guess.
    static func from(wire: String?) -> WitnessChainFormat? {
        guard let wire, !wire.isEmpty else { return .referenceV1 }
        return WitnessChainFormat(rawValue: wire)
    }
}

/// One record from the witness chain (spec/witness_api_v1.md §2.1).
/// reference_v1 hash = SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}")
/// — seven fields. time_source defaults to "device_clock"; gps_timestamp is
/// absent on the wire without a GPS fix and hashes as the empty string.
/// wap_v1 hash = SHA256("securacv:fw:chain:v1" ‖ 0x00 ‖ prev ‖ payload_hash ‖ seq(BE32) ‖ time_bucket(BE32)).
struct WitnessRecord: Identifiable, Codable, Hashable, Sendable {
    var seq: UInt64
    var hash: String
    var prevHash: String
    /// Coarse — presented as a ten-minute bucket (Invariant III). When the
    /// wire carried no timestamp this is the locally anchored bucket start
    /// (see `hasWireTimestamp`).
    var timestamp: Date
    var eventType: String
    var zone: String
    /// Hex Ed25519 signature. EMPTY means the record is not individually
    /// signed (absent or "" on the wire); ChainVerifier reads that as
    /// `.unsigned`, never as anything stronger.
    var signature: String
    /// "device_clock" or "gps_utc" — part of the reference_v1 signed pre-image.
    var timeSource: String = "device_clock"
    /// The GPS-derived UTC string when time_source is gps_utc; empty (and
    /// absent on the wire) otherwise. Part of the reference_v1 pre-image.
    var gpsTimestamp: String = ""
    /// wap_v1 pre-image fields (spec §4.2). Nil on reference_v1 pages; a
    /// wap_v1 record missing them cannot be recomputed and reads as a
    /// broken link.
    var payloadHash: String? = nil
    var timeBucket: UInt32? = nil
    var timeBucketMS: UInt32? = nil
    /// The raw firmware record-type enum, when the page carries it.
    var recordType: Int? = nil
    /// False when the wire carried no `timestamp` and the value above was
    /// anchored at decode time (WitnessChainPage.anchorMissingTimestamps).
    var hasWireTimestamp: Bool = true

    var id: UInt64 { seq }

    enum CodingKeys: String, CodingKey {
        case seq, hash
        case prevHash = "prev_hash"
        case timestamp
        case eventType = "event_type"
        case zone, signature
        case timeSource = "time_source"
        case gpsTimestamp = "gps_timestamp"
        case payloadHash = "payload_hash"
        case timeBucket = "time_bucket"
        case timeBucketMS = "time_bucket_ms"
        case recordType = "record_type"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        seq = try c.decode(UInt64.self, forKey: .seq)
        hash = try c.decode(String.self, forKey: .hash)
        prevHash = try c.decode(String.self, forKey: .prevHash)
        eventType = try c.decode(String.self, forKey: .eventType)
        // The contract's optional fields, each with the meaning its absence
        // carries: no zone is an empty zone; no signature is UNSIGNED; no
        // timestamp is "no clock yet" and is anchored by the page.
        zone = try c.decodeIfPresent(String.self, forKey: .zone) ?? ""
        signature = try c.decodeIfPresent(String.self, forKey: .signature) ?? ""
        if let wireTimestamp = try c.decodeIfPresent(Date.self, forKey: .timestamp) {
            timestamp = wireTimestamp
            hasWireTimestamp = true
        } else {
            timestamp = .distantPast
            hasWireTimestamp = false
        }
        // Wire defaults match device-state.js: time_source is always written
        // but tolerate its absence; gps_timestamp is omitted without a fix.
        timeSource = try c.decodeIfPresent(String.self, forKey: .timeSource) ?? "device_clock"
        gpsTimestamp = try c.decodeIfPresent(String.self, forKey: .gpsTimestamp) ?? ""
        payloadHash = try c.decodeIfPresent(String.self, forKey: .payloadHash)
        timeBucket = try c.decodeIfPresent(UInt32.self, forKey: .timeBucket)
        timeBucketMS = try c.decodeIfPresent(UInt32.self, forKey: .timeBucketMS)
        recordType = try c.decodeIfPresent(Int.self, forKey: .recordType)
    }

    init(seq: UInt64, hash: String, prevHash: String, timestamp: Date,
         eventType: String, zone: String, signature: String,
         timeSource: String = "device_clock", gpsTimestamp: String = "",
         payloadHash: String? = nil, timeBucket: UInt32? = nil,
         timeBucketMS: UInt32? = nil, recordType: Int? = nil,
         hasWireTimestamp: Bool = true) {
        self.seq = seq
        self.hash = hash
        self.prevHash = prevHash
        self.timestamp = timestamp
        self.eventType = eventType
        self.zone = zone
        self.signature = signature
        self.timeSource = timeSource
        self.gpsTimestamp = gpsTimestamp
        self.payloadHash = payloadHash
        self.timeBucket = timeBucket
        self.timeBucketMS = timeBucketMS
        self.recordType = recordType
        self.hasWireTimestamp = hasWireTimestamp
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(seq, forKey: .seq)
        try c.encode(hash, forKey: .hash)
        try c.encode(prevHash, forKey: .prevHash)
        // A locally anchored time is not something the device said; it does
        // not go back out as if it were.
        if hasWireTimestamp { try c.encode(timestamp, forKey: .timestamp) }
        try c.encode(eventType, forKey: .eventType)
        try c.encode(zone, forKey: .zone)
        try c.encode(signature, forKey: .signature)
        try c.encode(timeSource, forKey: .timeSource)
        if !gpsTimestamp.isEmpty { try c.encode(gpsTimestamp, forKey: .gpsTimestamp) }
        try c.encodeIfPresent(payloadHash, forKey: .payloadHash)
        try c.encodeIfPresent(timeBucket, forKey: .timeBucket)
        try c.encodeIfPresent(timeBucketMS, forKey: .timeBucketMS)
        try c.encodeIfPresent(recordType, forKey: .recordType)
    }

    /// The exact reference_v1 pre-image the device hashes — recomputed here
    /// to check the chain link locally. Kept in one place so it can't drift
    /// from the spec. Pinned against vectors produced by the device's own
    /// hashing code in WitnessChainPreimageTests — this string USED to omit
    /// the two trailing fields, which made every healthy device verify as
    /// tampered. (wap_v1 records are recomputed in ChainVerifier.wapChainHash.)
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

/// One `GET /api/v1/witness` page (spec/witness_api_v1.md §2). Every field
/// but `records` is optional on the wire — the reference device-api sends
/// only `records`, and its absent `chain_format` means reference_v1.
struct WitnessChainPage: Codable, Sendable {
    var records: [WitnessRecord]
    var schema: String? = nil
    /// "reference_v1" / "wap_v1"; resolve through `format`.
    var chainFormat: String? = nil
    var deviceID: String? = nil
    /// The chain length as the device counts it (newest seq issued).
    var total: UInt64? = nil
    /// Seconds since boot at render — the anchor for clockless records.
    var uptimeS: UInt64? = nil

    enum CodingKeys: String, CodingKey {
        case records, schema, total
        case chainFormat = "chain_format"
        case deviceID = "device_id"
        case uptimeS = "uptime_s"
    }

    init(records: [WitnessRecord], chainFormat: String? = nil, deviceID: String? = nil,
         total: UInt64? = nil, uptimeS: UInt64? = nil, schema: String? = nil) {
        self.records = records
        self.chainFormat = chainFormat
        self.deviceID = deviceID
        self.total = total
        self.uptimeS = uptimeS
        self.schema = schema
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        records = try c.decode([WitnessRecord].self, forKey: .records)
        schema = try c.decodeIfPresent(String.self, forKey: .schema)
        chainFormat = try c.decodeIfPresent(String.self, forKey: .chainFormat)
        deviceID = try c.decodeIfPresent(String.self, forKey: .deviceID)
        total = try c.decodeIfPresent(UInt64.self, forKey: .total)
        uptimeS = try c.decodeIfPresent(UInt64.self, forKey: .uptimeS)
        anchorMissingTimestamps(fetchedAt: Date())
    }

    /// The chain construction this page uses; nil for one this build cannot
    /// recompute (ChainVerifier answers `.unsupportedFormat`).
    var format: WitnessChainFormat? { WitnessChainFormat.from(wire: chainFormat) }

    /// Give every clockless record a COARSE local time (spec §3.2): the
    /// record's bucket start is `time_bucket × time_bucket_ms` ms after boot,
    /// the page was rendered `uptime_s` after boot and fetched at `now`, so
    /// the record is `uptime_s − start` seconds old — floored to the
    /// ten-minute grain, the same anchoring the WAP events feed uses
    /// (Wire/WapEvents.swift). A record with no bucket at all takes the
    /// fetch time's bucket: "no later than now" is the only honest bound.
    mutating func anchorMissingTimestamps(fetchedAt now: Date) {
        guard records.contains(where: { !$0.hasWireTimestamp }) else { return }
        for i in records.indices where !records[i].hasWireTimestamp {
            var when = now
            if let uptime = uptimeS,
               let bucket = records[i].timeBucket,
               let bucketMS = records[i].timeBucketMS {
                let startS = Double(bucket) * Double(bucketMS) / 1000
                let ageS = max(0, Double(uptime) - startS)
                when = now.addingTimeInterval(-ageS)
            }
            records[i].timestamp = Self.coarse(when)
        }
    }

    /// Ten-minute floor (Invariant III) — the same arithmetic as
    /// FleetStore.bucket, kept here so the model needs no store.
    static func coarse(_ date: Date) -> Date {
        let t = date.timeIntervalSince1970
        return Date(timeIntervalSince1970: (t / 600).rounded(.down) * 600)
    }
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
