// SenseModalityTests.swift
//
// The "how it senses" derivation, pinned two ways: its honesty rules (an
// unmapped type gets no glyph, never a guess), and its parity with the
// wire-contract home of the map — DEVICE_TYPE_MODALITY in the Home
// Assistant integration's const.py. This Swift copy is the map's third
// (const.py and the JS timeline card are the others) and the dictionary
// linter does not cover it, so the belt here reads const.py off disk: a
// pair added or changed there fails a test here instead of silently
// drifting. Same belt-over-the-repo pattern as the dictionary mirror and
// firmware type_name tests.

import XCTest
@testable import SecuraCV

final class SenseModalityTests: XCTestCase {

    func testKnownDeviceTypesMapAndUnknownOnesStaySilent() {
        XCTAssertEqual(SenseModality(publishedType: "canary-vision"), .camera)
        XCTAssertEqual(SenseModality(publishedType: "canary-sense"), .radar)
        XCTAssertEqual(SenseModality(publishedType: "canary-wap"), .wifiCSI)
        XCTAssertEqual(SenseModality(publishedType: "canary-contact"), .contact)
        // No glyph, never a guess — the const.py precedent. Displays watch
        // the fleet; they don't sense, and the row must not invent a sense.
        XCTAssertNil(SenseModality(publishedType: "canary-display"))
        XCTAssertNil(SenseModality(publishedType: "canary-nightstand"))
        XCTAssertNil(SenseModality(publishedType: nil))
    }

    func testRawValuesAreTheDictionaryModalityIDs() {
        XCTAssertEqual(SenseModality.camera.rawValue, "camera")
        XCTAssertEqual(SenseModality.radar.rawValue, "radar")
        XCTAssertEqual(SenseModality.wifiCSI.rawValue, "wifi-csi")
        XCTAssertEqual(SenseModality.contact.rawValue, "contact")
    }

    func testEveryModalityHasPresentation() {
        for m in [SenseModality.camera, .radar, .wifiCSI, .contact] {
            XCTAssertFalse(m.label.isEmpty)
            XCTAssertFalse(m.sfSymbol.isEmpty)
        }
    }

    func testTheSwiftCopyMirrorsConstPyOnDisk() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let constPy = repoRoot.appendingPathComponent("custom_components/securacv/const.py")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: constPy.path),
                          "repo checkout not visible from the test host")

        let source = try String(contentsOf: constPy, encoding: .utf8)
        // The map's block, then its pairs. MODALITY_* constant names map to
        // dictionary ids mechanically (lowercase, underscore → hyphen) —
        // the same relation const.py itself declares.
        let blockRegex = try NSRegularExpression(
            pattern: #"DEVICE_TYPE_MODALITY\s*=\s*\{([^}]*)\}"#)
        let fullRange = NSRange(source.startIndex..., in: source)
        let blockMatch = try XCTUnwrap(blockRegex.firstMatch(in: source, range: fullRange),
                                       "DEVICE_TYPE_MODALITY moved — update this belt")
        let block = String(source[try XCTUnwrap(Range(blockMatch.range(at: 1), in: source))])

        let pairRegex = try NSRegularExpression(pattern: #""([a-z-]+)":\s*MODALITY_([A-Z_]+)"#)
        let pairs = pairRegex.matches(in: block, range: NSRange(block.startIndex..., in: block))
            .compactMap { match -> (deviceType: String, modalityID: String)? in
                guard let d = Range(match.range(at: 1), in: block),
                      let m = Range(match.range(at: 2), in: block) else { return nil }
                return (String(block[d]),
                        String(block[m]).lowercased().replacingOccurrences(of: "_", with: "-"))
            }
        XCTAssertFalse(pairs.isEmpty, "no pairs parsed — update the regex")

        for (deviceType, modalityID) in pairs {
            XCTAssertEqual(SenseModality(publishedType: deviceType)?.rawValue, modalityID,
                           "const.py maps \(deviceType) → \(modalityID); the Swift copy disagrees")
        }
        // And nothing extra: a Swift-only mapping would be a claim const.py
        // (the wire-contract home) never blessed.
        let constTypes = Set(pairs.map(\.deviceType))
        for deviceType in ["canary-vision", "canary-sense", "canary-wap", "canary-contact"] {
            XCTAssertTrue(constTypes.contains(deviceType),
                          "\(deviceType) is mapped in Swift but no longer in const.py")
        }
    }

    // MARK: - the wellbeing card's pure helpers

    func testOccupantsLabelTopsOutHonestly() {
        XCTAssertEqual(Witness.occupantsLabel(0), "0")
        XCTAssertEqual(Witness.occupantsLabel(1), "1")
        XCTAssertEqual(Witness.occupantsLabel(2), "2+")
        // The radar's contract is 0/1/2-or-more; a bigger number must never
        // pretend the device can count a crowd.
        XCTAssertEqual(Witness.occupantsLabel(5), "2+")
    }

    func testWellbeingGateLightsForAnyReading() {
        var w = Witness(id: "a")
        XCTAssertFalse(w.hasWellbeingData)
        w.breathingLock = false
        XCTAssertTrue(w.hasWellbeingData, "a single reading — even a negative one — is data")

        var t = Witness(id: "b")
        t.tempC = 21.5
        XCTAssertTrue(t.hasWellbeingData)
    }
}
