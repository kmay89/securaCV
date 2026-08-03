// EventVocabularyTests.swift
//
// The one vocabulary of meaning, tested three ways: it mirrors the witness
// dictionary exactly (belt to scripts/lint_dictionary_sync.py's braces), it
// keeps the device dialect's long-standing coarse meanings, and — the
// anti-rot heart — it renders vocabulary that DOESN'T EXIST YET as readable,
// calm words instead of breaking.

import XCTest
@testable import SecuraCV

final class EventVocabularyTests: XCTestCase {
    // MARK: - dictionary mirror

    func testEveryDictionaryIdRoundTripsThroughBothWireForms() {
        for kind in WitnessEvent.allCases {
            XCTAssertEqual(WitnessEvent(wire: kind.rawValue), kind)
            // The Rust wire form is the mechanical PascalCase of the id.
            let pascal = kind.rawValue.split(separator: "_")
                .map { $0.prefix(1).uppercased() + $0.dropFirst() }
                .joined()
            XCTAssertEqual(WitnessEvent(wire: pascal), kind,
                           "PascalCase wire form \(pascal) must resolve")
        }
    }

    func testTheSwiftMirrorMatchesTheDictionaryOnDisk() throws {
        // #filePath → ios/Tests/SecuraCVTests/… → repo root is three up.
        // The python linter is the durable repo-wide gate; this belt runs
        // wherever the repo is present alongside the built tests.
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let dict = repoRoot.appendingPathComponent("spec/witness_dictionary.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: dict.path),
                          "repo checkout not visible from the test host; " +
                          "scripts/lint_dictionary_sync.py remains the primary gate")

        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: dict)) as? [String: Any]
        let eventTypes = (json?["event_types"] as? [String: Any])?["items"] as? [[String: Any]]
        let items = try XCTUnwrap(eventTypes)

        let dictionaryIDs = Set(items.compactMap { $0["id"] as? String })
        let swiftIDs = Set(WitnessEvent.allCases.map(\.rawValue))
        XCTAssertEqual(swiftIDs, dictionaryIDs)

        for item in items {
            let id = try XCTUnwrap(item["id"] as? String)
            let label = try XCTUnwrap(item["label"] as? String)
            XCTAssertEqual(WitnessEvent(rawValue: id)?.label, label,
                           "label for \(id) must mirror the dictionary verbatim")
        }
    }

    func testEveryKnownEventHasPresentation() {
        for kind in WitnessEvent.allCases {
            XCTAssertFalse(kind.label.isEmpty)
            XCTAssertFalse(kind.sfSymbol.isEmpty)
        }
        for kind in DeviceEvent.allCases {
            XCTAssertFalse(kind.sfSymbol.isEmpty)
        }
    }

    // MARK: - device dialect keeps its long-standing coarse meanings

    func testDeviceDialectSeverityParityWithTheOriginalMapping() {
        XCTAssertEqual(EventVocabulary.severity(forWire: "tamper"), .tamper)
        XCTAssertEqual(EventVocabulary.severity(forWire: "panic"), .tamper)
        XCTAssertEqual(EventVocabulary.severity(forWire: "chain_break"), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: "verify_failed"), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: "witness_lost"), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: "person_detected"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "motion_detected"), .notice)
    }

    func testDictionarySeveritiesStayCalm() {
        XCTAssertEqual(EventVocabulary.severity(forWire: "tamper_detected"), .tamper)
        XCTAssertEqual(EventVocabulary.severity(forWire: "presence_in_restricted_zone"), .warn)
        XCTAssertEqual(EventVocabulary.severity(forWire: "vehicle_presence_after_hours"), .warn)
        // Nothing an ordinary week produces escalates past notice.
        XCTAssertEqual(EventVocabulary.severity(forWire: "boundary_crossing_object_large"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "contact_state_change"), .notice)
    }

    // MARK: - headlines

    func testDeviceDialectKeepsItsFriendlyPhrasing() {
        XCTAssertEqual(EventVocabulary.headline(forWire: "person_detected",
                                                zone: "front door", deviceName: "Porch"),
                       "Someone at front door")
        XCTAssertEqual(EventVocabulary.headline(forWire: "person_detected",
                                                zone: "", deviceName: "Porch"),
                       "Someone at Porch")
        XCTAssertEqual(EventVocabulary.headline(forWire: "tamper",
                                                zone: "yard", deviceName: "Shed Cam"),
                       "Tamper at Shed Cam")
    }

    func testDictionaryEventsReadAsTheirLabels() {
        XCTAssertEqual(EventVocabulary.headline(forWire: "boundary_crossing_object_large",
                                                zone: "driveway", deviceName: "Drive"),
                       "Large object crossed boundary · driveway")
        XCTAssertEqual(EventVocabulary.headline(forWire: "BoundaryCrossingObjectLarge",
                                                zone: "", deviceName: "Drive"),
                       "Large object crossed boundary")
    }

    // MARK: - the future never breaks

    func testUnknownVocabularyRendersCalmReadableWords() {
        XCTAssertEqual(EventVocabulary.severity(forWire: "solar_flare_detected"), .notice)
        XCTAssertEqual(EventVocabulary.headline(forWire: "solar_flare_detected",
                                                zone: "roof", deviceName: "Sky"),
                       "Solar flare detected · roof")
        XCTAssertEqual(EventVocabulary.sfSymbol(forWire: "solar_flare_detected"), "sparkle")
    }

    func testSnakeCaseConversion() {
        XCTAssertEqual(EventVocabulary.snakeCase("BoundaryCrossingObjectLarge"),
                       "boundary_crossing_object_large")
        XCTAssertEqual(EventVocabulary.snakeCase("already_snake"), "already_snake")
    }
}
