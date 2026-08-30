// ChirpAdvertTests.swift
//
// Pins the chirp decoder to the firmware's layout — which matters MORE here
// than for the beacon, because the chirp has no firmware host test of its
// own to lift vectors from: this suite's encode/parse round-trip plus the
// source belt at the bottom IS the cross-repo sync guard. If ble_chirp.h
// ever moves a byte, the belt names the drift before a phone misreads a
// tamper cry in the field.

import XCTest
@testable import SecuraCV

final class ChirpAdvertTests: XCTestCase {

    private func blob(kind: ChirpAdvert.Kind = .alert) -> Data {
        ChirpAdvert.encode(kind: kind, hourBucket: 0x0102_0304,
                           chainHashPrefix: [9, 8, 7, 6, 5, 4, 3, 2],
                           fpB0: 0xA3, fpB1: 0xF7)
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
    /// EventVocabulary and WapEvents belts.
    func testFirmwareChirpLayoutStillMatchesThisDecoder() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let sketch = root.appendingPathComponent(
            "firmware/projects/canary-wap/arduino/canary_wap")
        let chirpH = sketch.appendingPathComponent("ble_chirp.h")
        let configH = sketch.appendingPathComponent("ble_config.h")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: chirpH.path),
                          "firmware sources not visible from this checkout")
        let chirpSrc = try String(contentsOf: chirpH, encoding: .utf8)
        let configSrc = try String(contentsOf: configH, encoding: .utf8)

        // 15 payload bytes + the 2-byte company id NimBLE prepends = our 17.
        XCTAssertTrue(configSrc.contains("#define CHIRP_PAYLOAD_SIZE  15"),
                      "the chirp payload size moved — update ChirpAdvert.mfgLength and this belt")
        XCTAssertTrue(configSrc.contains("0xFFFF"),
                      "the chirp company id moved off the BLE test id")
        // The five kinds, at the exact wire bytes this decoder maps.
        for pin in ["CHIRP_ALERT     = 0x01", "CHIRP_HEARTBEAT = 0x02",
                    "CHIRP_TAMPER    = 0x03", "CHIRP_WITNESS   = 0x04",
                    "CHIRP_BOOT      = 0x05"] {
            XCTAssertTrue(configSrc.contains(pin), "chirp kind table moved: \(pin)")
        }
        // The bucket is boot-relative millis, whatever the draft doc says —
        // the reason hourBucket must never render as a time of day.
        XCTAssertTrue(chirpSrc.contains("millis() / 3600000UL"),
                      "the chirp timestamp stopped being boot-relative — revisit hourBucket's caveat")
        // The fingerprint bytes sit at payload [13-14] (on-air [15-16]).
        XCTAssertTrue(chirpSrc.contains("payload[13] = g_deviceIdPrefixBytes[0]"),
                      "the fingerprint bytes moved — the suffix decode reads air bytes 15-16")
    }
}
