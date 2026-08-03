// ConsentAndLivenessTests.swift
//
// Two small contracts with outsized consequences:
//   * Consent is tri-state — "never asked" is real, distinct from "no", so
//     the UI can invite once, stay findable after a no, and nag on neither.
//   * The liveness ladder is fast but never jumpy — one miss is weather,
//     two reads "Quiet", three reads "Lost". At a 5-second cadence that is
//     seconds-fast offline detection with zero cloud in the loop.

import XCTest
@testable import SecuraCV

final class ConsentAndLivenessTests: XCTestCase {
    func testConsentIsHonestlyTriState() throws {
        let suite = "test-consents-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        XCTAssertNil(Consents.discovery(in: defaults), "never asked ≠ no")
        Consents.setDiscovery(false, in: defaults)
        XCTAssertEqual(Consents.discovery(in: defaults), false)
        Consents.setDiscovery(true, in: defaults)
        XCTAssertEqual(Consents.discovery(in: defaults), true)
    }

    func testTheLadderIsFastButNeverJumpy() {
        XCTAssertEqual(LivenessLadder.link(afterMisses: 0), .online)
        XCTAssertEqual(LivenessLadder.link(afterMisses: 1), .online,
                       "one miss is weather, not news")
        XCTAssertEqual(LivenessLadder.link(afterMisses: 2), .stale)
        XCTAssertEqual(LivenessLadder.link(afterMisses: 3), .lost)
        XCTAssertEqual(LivenessLadder.link(afterMisses: 12), .lost)
    }

    func testALostWitnessIsAnAlertCondition() {
        // The ladder's "lost" must flow into the severity that alerts and
        // escalates every glance surface — that's the whole point of speed.
        var w = Witness(id: "canary-a")
        w.link = LivenessLadder.link(afterMisses: LivenessLadder.lostAfterMisses)
        XCTAssertTrue(w.link.isDark)
        XCTAssertEqual(w.effectiveSeverity, .alert)
    }
}
