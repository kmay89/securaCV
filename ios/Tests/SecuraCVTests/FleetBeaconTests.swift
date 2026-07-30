// FleetBeaconTests.swift
//
// The app's half of the fleet-link beacon sync guard.
//
// The firmware keeps its copies of `fleet_beacon.h` byte-identical with
// `firmware/scripts/check_fleet_beacon_sync.sh`. A Swift port can't be a
// byte-copy of a C header, so the discipline here is the TEST: these vectors
// are lifted verbatim from `firmware/common/fleet_link/test_fleet_beacon.cpp`.
// If the wire format ever changes, that C test changes — and this one must fail
// until it changes with it. That is what stops the two implementations drifting
// while both keep passing their own suites.

import XCTest
@testable import SecuraCV

final class FleetBeaconTests: XCTestCase {

    // ── Nominal build: known values, LE chain, fp bytes ──
    // Mirrors the C test's first block exactly.
    func testNominalEncodeMatchesFirmwareVector() {
        let flags = FleetBeacon.flagTamper | FleetBeacon.flagOnWiFiSTA
        let blob = FleetBeacon.encode(flags: flags, batteryPct: 76, healthPct: 88,
                                      chainHeight: 0x1234, fpB0: 0xAB, fpB1: 0xCD)
        let b = [UInt8](blob)

        XCTAssertEqual(b.count, 11, "full on-air blob is 11 bytes (9 payload + 2 company)")
        XCTAssertEqual(b[0], 0xFF, "company id low byte")
        XCTAssertEqual(b[1], 0xFF, "company id high byte")
        XCTAssertEqual(b[2], 0x10, "type byte is 0x10 (BEACON_PRESENCE)")
        XCTAssertEqual(b[3], 0x01, "schema version is 0x01")
        XCTAssertEqual(b[4], flags, "flags byte round-trips (tamper|on_wifi_sta)")
        XCTAssertEqual(b[5], 76, "battery byte")
        XCTAssertEqual(b[6], 88, "health byte")
        XCTAssertEqual(b[7], 0x34, "chain_lo low byte (LE)")
        XCTAssertEqual(b[8], 0x12, "chain_lo high byte (LE)")
        XCTAssertEqual(b[9], 0xAB, "fp byte 0")
        XCTAssertEqual(b[10], 0xCD, "fp byte 1")
    }

    // ── Unknown battery/health map to 0xFF, and back to nil ──
    func testUnknownPercentagesUseTheSentinel() {
        XCTAssertEqual(FleetBeacon.pct(nil), 0xFF, "nil -> 0xFF unknown")
        XCTAssertEqual(FleetBeacon.pct(-1), 0xFF, "battery <0 -> 0xFF unknown")
        XCTAssertEqual(FleetBeacon.pct(101), 0xFF, "battery >100 -> 0xFF unknown")
        XCTAssertEqual(FleetBeacon.pct(200), 0xFF, "health >100 -> 0xFF unknown")
        XCTAssertEqual(FleetBeacon.pct(0), 0, "battery 0 is valid")
        XCTAssertEqual(FleetBeacon.pct(100), 100, "health 100 is valid")
    }

    /// The sentinel must decode back to nil, never to 255 — "not published"
    /// has to stay distinct from a real reading.
    func testSentinelDecodesToNilNotToTwoFiftyFive() {
        let blob = FleetBeacon.encode(flags: 0, batteryPct: nil, healthPct: nil,
                                      chainHeight: 0, fpB0: 0, fpB1: 0)
        let parsed = FleetBeacon.parse(manufacturerData: blob)
        XCTAssertNotNil(parsed)
        XCTAssertNil(parsed?.batteryPct, "0xFF battery decodes to unknown, not 255")
        XCTAssertNil(parsed?.healthPct, "0xFF health decodes to unknown, not 255")
    }

    // ── Only the low 16 bits of chain_height ride the wire ──
    func testChainHeightTruncatesToLowSixteenBits() {
        let blob = FleetBeacon.encode(flags: 0, batteryPct: 50, healthPct: 50,
                                      chainHeight: 0xDEAD_0042, fpB0: 0, fpB1: 0)
        let b = [UInt8](blob)
        XCTAssertEqual(b[7], 0x42, "chain low byte from a >16-bit height")
        XCTAssertEqual(b[8], 0x00, "chain high byte truncated to low 16 bits")
        XCTAssertEqual(FleetBeacon.parse(manufacturerData: blob)?.chainLow16, 0x0042)
    }

    // ── Round-trip build -> full-blob parse ──
    func testRoundTrip() {
        let flags = FleetBeacon.flagDegraded | FleetBeacon.flagAlert | FleetBeacon.flagMicMuted
        let blob = FleetBeacon.encode(flags: flags, batteryPct: 42, healthPct: 99,
                                      chainHeight: 0xBEEF, fpB0: 0x1A, fpB1: 0x2B)
        let p = FleetBeacon.parse(manufacturerData: blob)
        XCTAssertEqual(p?.flags, flags)
        XCTAssertEqual(p?.batteryPct, 42)
        XCTAssertEqual(p?.healthPct, 99)
        XCTAssertEqual(p?.chainLow16, 0xBEEF)
        XCTAssertEqual(p?.fpB0, 0x1A)
        XCTAssertEqual(p?.fpB1, 0x2B)

        XCTAssertEqual(p?.degraded, true)
        XCTAssertEqual(p?.alertActive, true)
        XCTAssertEqual(p?.micMuted, true)
        XCTAssertEqual(p?.tamper, false, "a flag that wasn't set must stay false")
        XCTAssertEqual(p?.onWiFiSTA, false)
    }

    // ── The parser must reject anything that isn't ours ──
    // A phone in a city hears hundreds of adverts a second; every one of them
    // reaches `didDiscover`. Mis-parsing one as a Canary would invent a device.
    func testRejectsForeignAdvertisements() {
        var good = [UInt8](FleetBeacon.encode(flags: 0, batteryPct: 1, healthPct: 1,
                                              chainHeight: 1, fpB0: 1, fpB1: 1))

        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data(good.dropLast())),
                     "a short blob is not a beacon")
        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data(good + [0x00])),
                     "a long blob is not a beacon")
        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data()),
                     "empty manufacturer data is not a beacon")

        var wrongCompany = good; wrongCompany[0] = 0x4C; wrongCompany[1] = 0x00   // Apple
        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data(wrongCompany)),
                     "another vendor's company id must be rejected")

        var wrongType = good; wrongType[2] = 0x01     // a chirp, not a presence beacon
        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data(wrongType)),
                     "the chirp type must not parse as a presence beacon")

        var wrongVersion = good; wrongVersion[3] = 0x02
        XCTAssertNil(FleetBeacon.parse(manufacturerData: Data(wrongVersion)),
                     "an unknown schema version must be rejected, not guessed at")

        good[2] = FleetBeacon.type
        XCTAssertNotNil(FleetBeacon.parse(manufacturerData: Data(good)),
                        "the untouched vector still parses (the negatives above were real changes)")
    }

    /// `CBAdvertisementDataManufacturerDataKey` can hand back a slice whose
    /// indices don't start at zero. Parsing must not depend on that.
    func testParsesASlicedData() {
        let blob = FleetBeacon.encode(flags: FleetBeacon.flagTamper, batteryPct: 7, healthPct: 8,
                                      chainHeight: 9, fpB0: 0x0A, fpB1: 0x0B)
        let padded = Data([0xDE, 0xAD]) + blob
        let slice = padded.dropFirst(2)          // startIndex == 2, not 0
        let p = FleetBeacon.parse(manufacturerData: slice)
        XCTAssertEqual(p?.batteryPct, 7, "a re-based Data slice must parse identically")
        XCTAssertEqual(p?.tamper, true)
    }

    // ── Identity helpers ──
    func testFingerprintSuffixAndProvisionalName() {
        let b = FleetBeacon.parse(manufacturerData:
            FleetBeacon.encode(flags: 0, batteryPct: nil, healthPct: nil,
                               chainHeight: 0, fpB0: 0xAB, fpB1: 0xCD))!
        XCTAssertEqual(b.fingerprintSuffix, "abcd")
        XCTAssertEqual(b.provisionalName, "SCV-ABCD")
        XCTAssertTrue(b.matches(fingerprint: "0011223344556677889900aabbccABCD"),
                      "suffix match is case-insensitive")
        XCTAssertFalse(b.matches(fingerprint: "0011223344556677889900aabbcc0000"))
        XCTAssertFalse(b.matches(fingerprint: ""), "an unknown fingerprint never matches")
    }
}
