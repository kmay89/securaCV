// ChirpAdvertTests.swift
//
// Pins the chirp decoder to the firmware's layout. The firmware spells it
// once, in firmware/common/fleet_link/fleet_chirp.h (the canary-wap builds
// with it, every firmware receiver parses with it), and host-tests it in
// test_fleet_chirp.cpp beside it: this suite decodes that test's golden
// vector, round-trips encode/parse, and its source belt at the bottom reads
// the header and the sender's lines over it. If the layout ever moves a
// byte, the belt names the drift before a phone misreads a tamper cry in
// the field.

import XCTest
@testable import SecuraCV

final class ChirpAdvertTests: XCTestCase {

    private func blob(kind: ChirpAdvert.Kind = .alert) -> Data {
        ChirpAdvert.encode(kind: kind, hourBucket: 0x0102_0304,
                           chainHashPrefix: [9, 8, 7, 6, 5, 4, 3, 2],
                           fpB0: 0xA3, fpB1: 0xF7)
    }

    /// The golden vector firmware/common/fleet_link/test_fleet_chirp.cpp
    /// pins for fleet_chirp_build() (what the canary-wap has always put on
    /// air): a heartbeat, hour bucket 0x01020304, chain prefix 11..88,
    /// fingerprint suffix ab cd, behind the 0xFFFF company id.
    func testDecodesTheFirmwareHostTestsGoldenVector() throws {
        let golden: [UInt8] = [0xFF, 0xFF, 0x02, 0x01, 0x02, 0x03, 0x04,
                               0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                               0xAB, 0xCD]
        let chirp = try XCTUnwrap(ChirpAdvert.parse(manufacturerData: Data(golden)))
        XCTAssertEqual(chirp.kind, .heartbeat)
        XCTAssertEqual(chirp.hourBucket, 0x0102_0304)
        XCTAssertEqual(chirp.chainHashPrefix, [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
        XCTAssertEqual(chirp.fingerprintSuffix, "abcd")
        XCTAssertEqual([UInt8](ChirpAdvert.encode(kind: .heartbeat, hourBucket: 0x0102_0304,
                                                  chainHashPrefix: chirp.chainHashPrefix,
                                                  fpB0: 0xAB, fpB1: 0xCD)), golden)
    }

    func testEveryKindRoundTripsThroughTheWireLayout() throws {
        for kind in ChirpAdvert.Kind.allCases {
            let chirp = try XCTUnwrap(ChirpAdvert.parse(manufacturerData: blob(kind: kind)),
                                      "\(kind.rawValue) failed to parse its own bytes")
            XCTAssertEqual(chirp.kind, kind)
            XCTAssertEqual(chirp.hourBucket, 0x0102_0304, "hour bucket is big-endian on this wire")
            XCTAssertEqual(chirp.chainHashPrefix, [9, 8, 7, 6, 5, 4, 3, 2])
            XCTAssertEqual(chirp.fingerprintSuffix, "a3f7")
            XCTAssertEqual(chirp.provisionalName, "SCV-A3F7")
        }
    }

    func testRejectsEverythingThatIsNotAChirp() {
        // Wrong lengths — including the beacon's own two, which the display
        // parser branches on the same way.
        for count in [11, 13, 16, 18] {
            XCTAssertNil(ChirpAdvert.parse(manufacturerData: Data(repeating: 0xFF, count: count)))
        }
        // Wrong company id.
        var wrongCompany = [UInt8](blob())
        wrongCompany[0] = 0x4C
        XCTAssertNil(ChirpAdvert.parse(manufacturerData: Data(wrongCompany)))
        // A type byte outside the vocabulary — 0x00 and the beacon's 0x10.
        for badType: UInt8 in [0x00, 0x06, 0x10] {
            var b = [UInt8](blob())
            b[2] = badType
            XCTAssertNil(ChirpAdvert.parse(manufacturerData: Data(b)),
                         "type 0x\(String(badType, radix: 16)) is not a chirp")
        }
    }

    /// Data slices carry a non-zero startIndex; the parser must read a
    /// sliced blob identically (the FleetBeacon contiguous-copy lesson).
    func testParsesASlicedDataWithoutTrapping() throws {
        var padded = Data([0xDE, 0xAD])
        padded.append(blob())
        let sliced = padded.dropFirst(2)
        let chirp = try XCTUnwrap(ChirpAdvert.parse(manufacturerData: sliced))
        XCTAssertEqual(chirp.fingerprintSuffix, "a3f7")
    }

    func testSuffixMatchIsAHintWithTheBeaconsExactSemantics() throws {
        let chirp = try XCTUnwrap(ChirpAdvert.parse(manufacturerData: blob()))
        XCTAssertTrue(chirp.matches(fingerprint: "0011223344556677889900aabbcca3f7"))
        XCTAssertFalse(chirp.matches(fingerprint: "0011223344556677889900aabbccffff"))
        XCTAssertFalse(chirp.matches(fingerprint: "a3f"), "fewer than 4 hex chars can match nothing")
    }

    // ── the source belt: the firmware's layout, read off disk ──

    /// The load-bearing facts of the chirp wire, pinned as source text so a
    /// firmware edit that moves a byte fails HERE, in the repo, before any
    /// phone meets it on the air. Same #filePath discipline as the
    /// EventVocabulary and WapEvents belts. fleet_chirp.h is the layout; the
    /// canary-wap's ble_config.h names its constants over it and ble_chirp.h
    /// builds with it.
    func testFirmwareChirpLayoutStillMatchesThisDecoder() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let wireH = root.appendingPathComponent("firmware/common/fleet_link/fleet_chirp.h")
        let sketch = root.appendingPathComponent(
            "firmware/projects/canary-wap/arduino/canary_wap")
        let chirpH = sketch.appendingPathComponent("ble_chirp.h")
        let configH = sketch.appendingPathComponent("ble_config.h")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: wireH.path),
                          "firmware sources not visible from this checkout")
        let wireSrc = try String(contentsOf: wireH, encoding: .utf8)
        let chirpSrc = try String(contentsOf: chirpH, encoding: .utf8)
        let configSrc = try String(contentsOf: configH, encoding: .utf8)

        // 15 payload bytes + the 2-byte company id in front = our 17.
        XCTAssertTrue(wireSrc.contains("#define FLEET_CHIRP_PAYLOAD_LEN  15"),
                      "the chirp payload size moved — update ChirpAdvert.mfgLength and this belt")
        XCTAssertTrue(wireSrc.contains("#define FLEET_CHIRP_MFG_LEN      17"),
                      "the chirp length moved — update ChirpAdvert.mfgLength and this belt")
        XCTAssertTrue(wireSrc.contains("#define FLEET_CHIRP_COMPANY_ID   0xFFFF"),
                      "the chirp company id moved off the BLE test id")
        // The five kinds, at the exact wire bytes this decoder maps.
        for pin in ["#define FLEET_CHIRP_TYPE_ALERT      0x01",
                    "#define FLEET_CHIRP_TYPE_HEARTBEAT  0x02",
                    "#define FLEET_CHIRP_TYPE_TAMPER     0x03",
                    "#define FLEET_CHIRP_TYPE_WITNESS    0x04",
                    "#define FLEET_CHIRP_TYPE_BOOT       0x05"] {
            XCTAssertTrue(wireSrc.contains(pin), "chirp kind table moved: \(pin)")
        }
        // Where each field sits on air, and the bucket's byte order.
        for pin in ["#define FLEET_CHIRP_OFF_TYPE    2", "#define FLEET_CHIRP_OFF_HOUR    3",
                    "#define FLEET_CHIRP_OFF_CHAIN   7", "#define FLEET_CHIRP_CHAIN_LEN   8",
                    "#define FLEET_CHIRP_OFF_FP      15",
                    "out[FLEET_CHIRP_OFF_HOUR - at]     = (uint8_t)((hour_bucket >> 24) & 0xFF);"] {
            XCTAssertTrue(wireSrc.contains(pin), "the chirp layout moved: \(pin)")
        }
        // The sender speaks it: its names are the header's, its builder the
        // header's builder.
        for pin in ["#define CHIRP_PAYLOAD_SIZE  FLEET_CHIRP_PAYLOAD_LEN",
                    "#define CHIRP_COMPANY_ID          FLEET_CHIRP_COMPANY_ID",
                    "CHIRP_ALERT     = FLEET_CHIRP_TYPE_ALERT",
                    "CHIRP_HEARTBEAT = FLEET_CHIRP_TYPE_HEARTBEAT",
                    "CHIRP_TAMPER    = FLEET_CHIRP_TYPE_TAMPER",
                    "CHIRP_WITNESS   = FLEET_CHIRP_TYPE_WITNESS",
                    "CHIRP_BOOT      = FLEET_CHIRP_TYPE_BOOT"] {
            XCTAssertTrue(configSrc.contains(pin), "the sender's chirp names moved: \(pin)")
        }
        XCTAssertTrue(chirpSrc.contains("*len = fleet_chirp_build(payload, (uint8_t)type, hourBucket, g_chainHead,"),
                      "the sender stopped building with fleet_chirp.h — this belt no longer reads its bytes")
        // The bucket is boot-relative millis, whatever the draft doc says —
        // the reason hourBucket must never render as a time of day.
        XCTAssertTrue(chirpSrc.contains("millis() / 3600000UL"),
                      "the chirp timestamp stopped being boot-relative — revisit hourBucket's caveat")
    }
}
