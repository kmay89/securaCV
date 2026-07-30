// FleetBeacon.swift
//
// The app-side twin of the CANONICAL fleet-link presence beacon,
// `firmware/common/fleet_link/fleet_beacon.h`. Every Canary broadcasts this —
// canary-vision and canary-sense from their own `src/net/fleet_beacon_adv.cpp`,
// canary-wap/canary-display through `securacv_ble_status`, which installs it as
// the PRIMARY advertisement. It is the one status surface that needs **no MQTT
// broker and no home Wi-Fi**: a continuous BLE manufacturer-data advert.
//
// PURE by design — no CoreBluetooth, no Network, no SwiftUI. That is the whole
// point: like the firmware's `tests_host` cores, this parses bytes and nothing
// else, so `FleetBeaconTests` can round-trip it on the host. The transport glue
// (BLEConsole) is thin and sits on top.
//
// AUTHORITATIVE WIRE CONTRACT — the full manufacturer-data blob exactly as a
// scanner delivers it (the 2 company bytes included; NimBLE prepends them, and
// CoreBluetooth hands them back the same way), 11 bytes:
//
//   [0-1]  0xFF 0xFF     company id (little-endian)
//   [2]    0x10          BEACON_PRESENCE type (distinct from chirp 0x01..0x05)
//   [3]    0x01          schema version
//   [4]    flags         bit0=tamper, bit1=mic_muted, bit2=degraded,
//                        bit3=on_wifi_sta, bit4=alert_active
//   [5]    battery_pct   0..100, 0xFF = unknown/none
//   [6]    health_pct    0..100, 0xFF = unknown
//   [7-8]  chain_height  low 16 bits, uint16 little-endian
//   [9-10] fp2           last 2 bytes of the pubkey fingerprint
//
// PARITY DISCIPLINE (docs/FLEET_PARITY.md): the firmware keeps its copies
// byte-identical with `check_fleet_beacon_sync.sh`. A Swift file can't be a
// byte-copy of a C header, so the guard here is the TEST: FleetBeaconTests uses
// the same vectors as `firmware/common/fleet_link/test_fleet_beacon.cpp`. If the
// wire format changes, that C test changes — and this one must change with it.

import Foundation

/// One decoded presence beacon: the coarse, non-extractive state every Canary
/// broadcasts. Deliberately a plain value type so it can be cached, diffed, or
/// handed to a widget.
struct FleetBeacon: Hashable, Sendable {
    // ── wire constants (mirror fleet_beacon.h) ──
    static let companyID: UInt16 = 0xFFFF
    static let type: UInt8       = 0x10
    static let version: UInt8    = 0x01
    static let mfgLength         = 11    // FLEET_BEACON_MFG_LEN
    static let unknownPct: UInt8 = 0xFF

    static let flagTamper: UInt8     = 0x01
    static let flagMicMuted: UInt8   = 0x02
    static let flagDegraded: UInt8   = 0x04
    static let flagOnWiFiSTA: UInt8  = 0x08
    static let flagAlert: UInt8      = 0x10

    // ── decoded fields ──
    var flags: UInt8
    /// nil when the wire carried the 0xFF unknown sentinel — "not published"
    /// stays distinct from a real zero (the same rule the Witness model uses).
    var batteryPct: Int?
    var healthPct: Int?
    /// Only the low 16 bits of the chain height ride the wire.
    var chainLow16: UInt16
    var fpB0: UInt8
    var fpB1: UInt8

    var tamper: Bool     { flags & Self.flagTamper    != 0 }
    var micMuted: Bool   { flags & Self.flagMicMuted  != 0 }
    var degraded: Bool   { flags & Self.flagDegraded  != 0 }
    var onWiFiSTA: Bool  { flags & Self.flagOnWiFiSTA != 0 }
    var alertActive: Bool { flags & Self.flagAlert    != 0 }

    /// The 4 hex chars that end the device's pubkey fingerprint — the same two
    /// bytes the chirp carries and the "SCV-XXXX" display name derives from.
    var fingerprintSuffix: String {
        String(format: "%02x%02x", fpB0, fpB1)
    }

    /// Human label for a Canary we've heard but never paired with.
    var provisionalName: String {
        "SCV-" + fingerprintSuffix.uppercased()
    }

    /// True when this beacon plausibly belongs to a device whose full 16-hex
    /// fingerprint we already know. Two bytes is not an identity — it narrows,
    /// it does not prove — so callers must treat a match as a hint and never as
    /// authorization (pairing and chain verification remain the real gates).
    func matches(fingerprint: String) -> Bool {
        guard fingerprint.count >= 4 else { return false }
        return fingerprint.lowercased().hasSuffix(fingerprintSuffix)
    }

    /// Parse the FULL manufacturer-data blob, company bytes included — exactly
    /// what `CBAdvertisementDataManufacturerDataKey` delivers. Validates length,
    /// company id, type and version; returns nil on any mismatch so a stray
    /// advert from an unrelated device can never be read as a Canary.
    ///
    /// Mirrors `fleet_beacon_parse()` field for field.
    static func parse(manufacturerData mfg: Data) -> FleetBeacon? {
        guard mfg.count == mfgLength else { return nil }
        // Index through a contiguous copy: Data slices can carry a non-zero
        // startIndex, and subscripting those with 0-based offsets traps.
        let b = [UInt8](mfg)
        guard b[0] == 0xFF, b[1] == 0xFF else { return nil }   // company id (LE)
        guard b[2] == type else { return nil }
        guard b[3] == version else { return nil }
        return FleetBeacon(
            flags: b[4],
            batteryPct: b[5] == unknownPct ? nil : Int(b[5]),
            healthPct:  b[6] == unknownPct ? nil : Int(b[6]),
            chainLow16: UInt16(b[7]) | (UInt16(b[8]) << 8),
            fpB0: b[9],
            fpB1: b[10]
        )
    }

    /// Build the on-air blob. The app never advertises — this exists so the
    /// tests can round-trip against the firmware's vectors, which is what keeps
    /// the two implementations honest.
    static func encode(flags: UInt8, batteryPct: Int?, healthPct: Int?,
                       chainHeight: UInt32, fpB0: UInt8, fpB1: UInt8) -> Data {
        var b = [UInt8]()
        b.reserveCapacity(mfgLength)
        b.append(contentsOf: [0xFF, 0xFF])
        b.append(type)
        b.append(version)
        b.append(flags)
        b.append(pct(batteryPct))
        b.append(pct(healthPct))
        let low16 = UInt16(truncatingIfNeeded: chainHeight)
        b.append(UInt8(low16 & 0xFF))
        b.append(UInt8((low16 >> 8) & 0xFF))
        b.append(fpB0)
        b.append(fpB1)
        return Data(b)
    }

    /// Clamp a percentage to a 0...100 byte, mapping anything else to the 0xFF
    /// unknown sentinel — `fleet_beacon_pct()`.
    static func pct(_ value: Int?) -> UInt8 {
        guard let v = value, (0...100).contains(v) else { return unknownPct }
        return UInt8(v)
    }
}

/// One heard beacon plus the radio facts that came with it. Kept OUT of
/// `FleetBeacon` so the wire contract stays exactly the wire contract — RSSI and
/// timing are observations of a sighting, not part of the advertised payload.
///
/// Lives here rather than in the BLE transport so the merge that consumes it
/// (`FleetMerge`) needs no CoreBluetooth and stays host-testable.
struct BeaconSighting: Hashable, Sendable {
    var beacon: FleetBeacon
    var rssiDBM: Int
    var lastHeard: Date
    /// CoreBluetooth's per-device handle. Stable for a peripheral with a static
    /// MAC (which the ESP32s have), but it is a LOCAL handle — never an identity
    /// to trust. `beacon.fingerprintSuffix` is the device-asserted hint.
    var peripheralID: UUID
    /// Advertised local name, when the scan response carried one.
    var localName: String?

    var displayName: String { localName ?? beacon.provisionalName }

    /// Namespaced id for a Canary we've only ever heard. Distinct from a real
    /// `device_id` and from the demo fleet's `demo-` prefix, so a heard-only
    /// device can never be mistaken for a paired one.
    var provisionalID: String { "ble:" + peripheralID.uuidString }
}
