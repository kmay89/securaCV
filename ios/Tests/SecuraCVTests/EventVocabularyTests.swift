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

    // MARK: - the acoustic dialect (life-safety sounds land at honest severities)

    func testAcousticEventsLandAtHonestSeverities() {
        // Another alarm sounding in the home is an Alert — the same rung the
        // glass gives it. Before these cases existed, a heard smoke alarm
        // fell to the calm unknown-event fallback, which is exactly the
        // wrong place for it.
        XCTAssertEqual(EventVocabulary.severity(forWire: "smoke_alarm_t3"), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: "co_alarm_t4"), .alert)
        XCTAssertEqual(EventVocabulary.severity(forWire: "glass_break"), .warn)
        // Doorstep sounds and the mic audit stay everyday.
        XCTAssertEqual(EventVocabulary.severity(forWire: "knock"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "doorbell"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "mic_muted"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "mic_unmuted"), .notice)
    }

    func testAcousticHeadlinesSayHeardNeverVerified() {
        // "Heard": the device classified a cadence; it did not verify a
        // detector, and the sentence must not claim more than the ears did.
        XCTAssertEqual(EventVocabulary.headline(forWire: "smoke_alarm_t3",
                                                zone: "hallway", deviceName: "Hall"),
                       "Smoke alarm heard at hallway")
        XCTAssertEqual(EventVocabulary.headline(forWire: "co_alarm_t4",
                                                zone: "", deviceName: "Utility"),
                       "CO alarm heard at Utility")
        XCTAssertEqual(EventVocabulary.headline(forWire: "doorbell",
                                                zone: "", deviceName: "Entry"),
                       "Doorbell at Entry")
        XCTAssertEqual(EventVocabulary.headline(forWire: "mic_muted",
                                                zone: "kitchen", deviceName: "Kitchen"),
                       "Kitchen mic muted")
    }

    func testChokepointDialectsMirrorTheFirmwareTypeNamesOnDisk() throws {
        // The wire names are the firmware's own `type_name` strings in the
        // csi_event chokepoint modules — read them off disk so a renamed or
        // added event is a failing test here, not a silent fall to the
        // unknown-event fallback. Same belt-over-the-repo pattern as the
        // dictionary mirror test above. The sketch-tree copies are pinned
        // (they are byte-identical to firmware/common/csi/src, CI-enforced
        // by firmware/scripts/check_csi_sync.sh).
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let sketch = repoRoot.appendingPathComponent(
            "firmware/projects/canary-wap/arduino/canary_wap")
        let modules = [
            "acoustic_events_module.cpp",   // smoke/CO/knock/doorbell/glass + mic audit
            "vault_events_module.cpp",      // frame_sealed
            "tamper_events_module.cpp",     // tamper (per-kind via state)
            "core_presence.cpp",            // presence_changed
            "core_breathing.cpp",           // breathing_confirmed / breathing_lost
            "anomaly_baseline.cpp",         // unusual_motion / unusual_breathing
            "meta_daily_summary.cpp",       // daily_summary
            "core_activity_ribbon.cpp",     // ribbon_bucket_advanced
        ]
        try XCTSkipUnless(
            FileManager.default.fileExists(atPath: sketch.appendingPathComponent(modules[0]).path),
            "repo checkout not visible from the test host")

        let pattern = #"/\* type_name \*/\s*"([a-z0-9_]+)""#
        let regex = try NSRegularExpression(pattern: pattern)
        let known = Set(DeviceEvent.allCases.map(\.rawValue))

        for file in modules {
            let module = sketch.appendingPathComponent(file)
            XCTAssertTrue(FileManager.default.fileExists(atPath: module.path),
                          "\(file) moved — update this table")
            guard FileManager.default.fileExists(atPath: module.path) else { continue }
            let source = try String(contentsOf: module, encoding: .utf8)
            let range = NSRange(source.startIndex..., in: source)
            let firmwareNames = Set(regex.matches(in: source, range: range).compactMap {
                Range($0.range(at: 1), in: source).map { String(source[$0]) }
            })
            XCTAssertFalse(firmwareNames.isEmpty,
                           "\(file)'s type_name table moved — update the regex")
            for name in firmwareNames {
                XCTAssertTrue(known.contains(name),
                              "firmware event \(name) (\(file)) has no DeviceEvent case — " +
                              "it would render at the unknown-event fallback")
            }
        }
    }

    // MARK: - the wellbeing dialect (CSI/radar events, calm by doctrine)

    func testWellbeingEventsStayCalmExceptLearnedAnomalies() {
        // Everyday rhythm stays everyday…
        XCTAssertEqual(EventVocabulary.severity(forWire: "presence_changed"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "breathing_confirmed"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "breathing_lost"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "daily_summary"), .notice)
        XCTAssertEqual(EventVocabulary.severity(forWire: "ribbon_bucket_advanced"), .notice)
        // …and only a break from the device's own learned baseline asks for
        // a look — the after-hours-presence rung, never a siren.
        XCTAssertEqual(EventVocabulary.severity(forWire: "unusual_motion"), .warn)
        XCTAssertEqual(EventVocabulary.severity(forWire: "unusual_breathing"), .warn)
        // The vault's receipt is calm: the acoustic trigger that caused it
        // already alerted on its own row.
        XCTAssertEqual(EventVocabulary.severity(forWire: "frame_sealed"), .notice)
    }

    func testBreathingLostNeverReadsAsAMedicalClaim() {
        // A lost radar/CSI lock is a sensing fact (someone left the bed,
        // rolled over, walked out) — the sentence must say "no longer
        // sensed", never "stopped".
        let line = EventVocabulary.headline(forWire: "breathing_lost",
                                            zone: "nursery", deviceName: "Nursery")
        XCTAssertEqual(line, "Breathing no longer sensed at nursery")
        XCTAssertFalse(line.localizedCaseInsensitiveContains("stopped"))
    }

    func testFrameSealedReadsAsAReceipt() {
        XCTAssertEqual(EventVocabulary.headline(forWire: "frame_sealed",
                                                zone: "hallway", deviceName: "Hall"),
                       "A frame was sealed at hallway")
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

    /// TamperKind mirrors the integration's tamper vocabulary EXACTLY: every
    /// non-future const.py id narrates here, and nothing extra — an id this
    /// app invents is a tamper no device can raise, and a missing one is a
    /// kind the phone would flatten back to the bare "Tamper detected".
    /// Same const.py-off-disk discipline as the module belt above.
    func testTamperKindsMirrorTheIntegrationVocabulary() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let constPy = repoRoot.appendingPathComponent("custom_components/securacv/const.py")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: constPy.path),
                          "repo checkout not visible from the test host")
        let src = try String(contentsOf: constPy, encoding: .utf8)

        // Every TAMPER_<NAME> = "<id>" pair.
        let regex = try NSRegularExpression(pattern: #"(TAMPER_[A-Z_]+) = "([a-z_]+)""#)
        var idByName: [String: String] = [:]
        for m in regex.matches(in: src, range: NSRange(src.startIndex..., in: src)) {
            guard let n = Range(m.range(at: 1), in: src),
                  let v = Range(m.range(at: 2), in: src) else { continue }
            idByName[String(src[n])] = String(src[v])
        }
        XCTAssertFalse(idByName.isEmpty,
                       "const.py's tamper vocabulary moved — update this extraction")

        // The future fence lists constant NAMES the integration deliberately
        // does not advertise; those ids stay out of the app too.
        let fenceStart = try XCTUnwrap(src.range(of: "FUTURE_TAMPER_TYPES = ["))
        let fenceEnd = try XCTUnwrap(src.range(of: "]",
                                               range: fenceStart.upperBound..<src.endIndex))
        let fence = String(src[fenceStart.upperBound..<fenceEnd.lowerBound])
        let future = Set(idByName.filter { fence.contains($0.key) }.map(\.value))

        let expected = Set(idByName.values).subtracting(future)
        let ours = Set(TamperKind.allCases.map(\.rawValue))
        XCTAssertEqual(ours, expected,
                       "TamperKind and const.py diverged — move them together")
        for kind in TamperKind.allCases {
            XCTAssertFalse(kind.narration.isEmpty,
                           "\(kind.rawValue) owes one calm sentence")
        }
    }
}
