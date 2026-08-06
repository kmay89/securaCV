// ModelTests.swift
//
// Guards the two behaviors that keep the product honest: a mute can quiet the
// nagging but can NEVER hide tamper or a failed signature, and the push
// discipline never lets a mere digest event buzz someone.

import XCTest
@testable import SecuraCV

final class ModelTests: XCTestCase {

    func testMuteCapsRoutineButNotTamper() {
        var w = Witness(id: "canary-a3f7")
        w.lastEventSeverity = .warn
        w.mutedUntil = Date().addingTimeInterval(3600)
        XCTAssertEqual(w.effectiveSeverity, .notice, "a mute caps routine severity at notice")

        w.tamper = true
        XCTAssertEqual(w.effectiveSeverity, .tamper, "tamper must punch through a mute")
    }

    func testFailedSignaturePunchesThroughMute() {
        var w = Witness(id: "canary-b1c2")
        w.badge = .failed
        w.mutedUntil = Date().addingTimeInterval(3600)
        XCTAssertGreaterThanOrEqual(w.effectiveSeverity, .alert)
    }

    func testDarkLinkIsAtLeastAlert() {
        var w = Witness(id: "canary-d4e5")
        w.link = .lost
        XCTAssertGreaterThanOrEqual(w.effectiveSeverity, .alert, "a witness gone dark is an alert (dead-man's-switch)")
    }

    @MainActor
    func testDigestEventNeverPushes() {
        let center = isolatedCenter()
        // A notice-level event with default rules must not produce a push level.
        XCTAssertNil(center.level(for: .notice, awayFromHome: false),
                     "everyday activity is pull-only; it must never push")
    }

    @MainActor
    func testOnWiFiOnlyRuleIsSilentAwayFromHome() {
        let center = isolatedCenter()
        center.rules = [.init(id: "activity", title: "Activity", minSeverity: .notice, reach: .onWiFiOnly)]
        XCTAssertNil(center.level(for: .alert, awayFromHome: true),
                     "an on-Wi-Fi-only rule must stay silent when away")
    }

    @MainActor
    func testTamperIsCriticalWhenArmedAnywhere() {
        let center = isolatedCenter()
        XCTAssertEqual(center.level(for: .tamper, awayFromHome: true), .some(.critical))
    }

    func testSeverityOrdering() {
        XCTAssertTrue(Severity.ok < .notice)
        XCTAssertTrue(Severity.alert < .tamper)
        XCTAssertEqual([Severity.warn, .tamper, .ok].max(), .tamper)
    }
}

/// A center with its own defaults suite: AlertCenter persists what the user
/// arms, so tests that assign rules must not leave them for the next test.
@MainActor
private func isolatedCenter() -> AlertCenter {
    let suite = "test-alert-center-\(UUID().uuidString)"
    return AlertCenter(defaults: UserDefaults(suiteName: suite) ?? .standard)
}
