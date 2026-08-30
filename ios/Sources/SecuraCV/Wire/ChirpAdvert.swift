// ChirpAdvert.swift  (Wire/ — pure parsing, app target only)
//
// The BLE Chirp: a Canary's short, connectionless broadcast ALERT — the
// third and most transient thing a Canary says over the air, beside the
// presence beacon (FleetBeacon) and the GATT console. A chirping device
// swaps its advert for 2 seconds (ble_chirp.h CHIRP_BROADCAST_DURATION_MS)
// and then restores the beacon, so a chirp is a moment, not a presence.
//
// WIRE SHAPE, from the firmware that sends it and the display firmware that
// already parses it — the code, not docs/ble_protocol.md (Draft v0.1), is
// the contract (canary-wap ble_chirp.h buildChirpPayload + ble_config.h;
// canary-display chirp_scan.cpp handle_advert):
//
//   full manufacturer blob = 17 bytes, exactly:
//     [0-1]   company id 0xFF 0xFF (the BLE test id — see the caveat below)
//     [2]     chirp type, 0x01…0x05 (alert/heartbeat/tamper/witness/boot)
//     [3-6]   hour bucket, uint32 BIG-endian — BOOT-RELATIVE: the sender
//             uses millis()/3600000, not epoch time, whatever the draft doc
//             says. Parsed but never rendered as wall-clock time; the
//             display ignores it too and stamps arrival locally.
//     [7-14]  witness chain head hash prefix (8 bytes) — proves the sender
//             HAS a chain, verifies nothing (no signature on this advert)
//     [15-16] pubkey-fingerprint suffix, the same two bytes the beacon
//             carries — a hint that narrows, never an identity
//
// TRUST: none. Company id 0xFFFF is the Bluetooth SIG test id, the payload
// is unsigned with no checksum, and two fingerprint bytes are spoofable by
// anyone with a radio. A chirp may therefore feed LIVENESS and raise
// safety-positive attention (tamper), and must never set a badge, a name,
// or anything wearing the word "verified" — the FleetMerge rules.
//
// Three chirps exist in this project; this file is only the BLE one. The
// ESP-NOW community Chirp Channel (spec/chirp_channel_v0.md) is a different
// radio an iPhone cannot hear, and the audible identify chirp in
// FindCanaryView is a sound. No microphone is involved here — this is a
// Bluetooth scan, same permission story as the beacon.

import Foundation

/// One decoded chirp advert.
struct ChirpAdvert: Hashable, Sendable {
    /// The chirp vocabulary, raw values matching the firmware's
    /// chirpTypeName() strings (ble_config.h) so logs and tests speak the
    /// wire's own words.
    enum Kind: String, CaseIterable, Sendable {
        case alert, heartbeat, tamper, witness, boot

        init?(wireByte: UInt8) {
            switch wireByte {
            case 0x01: self = .alert
            case 0x02: self = .heartbeat
            case 0x03: self = .tamper
            case 0x04: self = .witness
            case 0x05: self = .boot
            default: return nil
            }
        }

        var wireByte: UInt8 {
            switch self {
            case .alert: return 0x01
            case .heartbeat: return 0x02
            case .tamper: return 0x03
            case .witness: return 0x04
            case .boot: return 0x05
            }
        }
    }

    var kind: Kind
    /// BOOT-RELATIVE hour bucket (millis()/3600000 on the sender). Exposed
    /// because the wire carries it; never render it as a time of day — the
    /// sender has no epoch clock, and the display precedent is to ignore it
    /// and stamp arrival locally.
    var hourBucket: UInt32
    /// 8-byte prefix of the sender's witness chain head hash. Unsigned and
    /// unverifiable from here — an existence claim, not evidence.
    var chainHashPrefix: [UInt8]
    var fpB0: UInt8
    var fpB1: UInt8

    static let mfgLength = 17

    /// Same conventions as FleetBeacon — the two bytes end the fingerprint.
    var fingerprintSuffix: String { String(format: "%02x%02x", fpB0, fpB1) }
    var provisionalName: String { "SCV-" + fingerprintSuffix.uppercased() }

    /// A hint that narrows, never an identity — the FleetBeacon rule,
    /// verbatim, for the same two bytes.
    func matches(fingerprint: String) -> Bool {
        guard fingerprint.count >= 4 else { return false }
        return fingerprint.lowercased().hasSuffix(fingerprintSuffix)
    }

    /// Parse the FULL manufacturer-data blob, company bytes included —
    /// exactly what `CBAdvertisementDataManufacturerDataKey` delivers.
    /// Returns nil on any mismatch (wrong length, wrong company id, a type
    /// byte outside the vocabulary) so a stray advert can never be read as
    /// a chirp. Mirrors chirp_scan.cpp's handle_advert gate byte for byte.
    static func parse(manufacturerData mfg: Data) -> ChirpAdvert? {
        guard mfg.count == mfgLength else { return nil }
        // Contiguous copy: Data slices can carry a non-zero startIndex, and
        // subscripting those with 0-based offsets traps (the FleetBeacon
        // lesson, kept).
        let b = [UInt8](mfg)
        guard b[0] == 0xFF, b[1] == 0xFF else { return nil }   // company id (LE)
        guard let kind = Kind(wireByte: b[2]) else { return nil }
        let bucket = (UInt32(b[3]) << 24) | (UInt32(b[4]) << 16)
                   | (UInt32(b[5]) << 8) | UInt32(b[6])
        return ChirpAdvert(
            kind: kind,
            hourBucket: bucket,
            chainHashPrefix: Array(b[7...14]),
            fpB0: b[15],
            fpB1: b[16]
        )
    }

    /// Build the on-air blob. The app never advertises — this exists so the
    /// tests can round-trip parse(encode(x)) == x against the firmware's
    /// layout. (There is no firmware host test for the chirp payload, unlike
    /// the beacon, so this round-trip plus the layout comment above IS the
    /// sync guard; a firmware layout change must update both.)
    static func encode(kind: Kind, hourBucket: UInt32,
                       chainHashPrefix: [UInt8], fpB0: UInt8, fpB1: UInt8) -> Data {
        var b = [UInt8](repeating: 0, count: mfgLength)
        b[0] = 0xFF; b[1] = 0xFF
        b[2] = kind.wireByte
        b[3] = UInt8((hourBucket >> 24) & 0xFF)
        b[4] = UInt8((hourBucket >> 16) & 0xFF)
        b[5] = UInt8((hourBucket >> 8) & 0xFF)
        b[6] = UInt8(hourBucket & 0xFF)
        for i in 0..<8 { b[7 + i] = i < chainHashPrefix.count ? chainHashPrefix[i] : 0 }
        b[15] = fpB0; b[16] = fpB1
        return Data(b)
    }
}

/// One heard chirp, with the local scan context — the ChirpAdvert twin of
/// BeaconSighting, kept beside it in Wire/ so FleetMerge stays free of
/// CoreBluetooth.
struct ChirpSighting: Hashable, Sendable {
    var chirp: ChirpAdvert
    var rssiDBM: Int
    var lastHeard: Date
    /// CoreBluetooth's per-device handle — a LOCAL handle, never an identity
    /// to trust (the BeaconSighting note).
    var peripheralID: UUID
    var localName: String?

    var displayName: String { localName ?? chirp.provisionalName }

    /// SAME namespace as BeaconSighting.provisionalID on purpose: a chirp
    /// replaces the sender's beacon on air for 2 seconds, so both sightings
    /// come from one peripheral — one namespace means one provisional row,
    /// never a phantom twin.
    var provisionalID: String { "ble:" + peripheralID.uuidString }
}
