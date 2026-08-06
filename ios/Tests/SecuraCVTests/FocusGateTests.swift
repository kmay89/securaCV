// FocusGateTests.swift
//
// The one bit the user's Focus writes, and the rule it must never bend:
// Focus quiets the everyday, never the smoke alarm.

import XCTest
@testable import SecuraCV

final class FocusGateTests: XCTestCase {
    func testTheGateReadsWhatTheFocusFilterWrote() throws {
        let suite = "test-focus-gate-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        XCTAssertFalse(FocusGate.criticalOnly(in: defaults))
        FocusGate.setCriticalOnly(true, in: defaults)
        XCTAssertTrue(FocusGate.criticalOnly(in: defaults))
        FocusGate.setCriticalOnly(false, in: defaults)
        XCTAssertFalse(FocusGate.criticalOnly(in: defaults))
    }

    @MainActor
    func testFocusQuietsImportantButNeverCritical() {
        let center = isolatedCenter()
        // Ensure a clean shared gate, and restore it whatever happens —
        // level(for:) reads the shared suite by design (the Focus intent may
        // run in another process).
        let wasOn = FocusGate.criticalOnly()
        defer { FocusGate.setCriticalOnly(wasOn) }

        FocusGate.setCriticalOnly(false)
        XCTAssertEqual(center.level(for: .alert, awayFromHome: false), .important)
        XCTAssertEqual(center.level(for: .tamper, awayFromHome: false), .critical)

        FocusGate.setCriticalOnly(true)
        XCTAssertNil(center.level(for: .alert, awayFromHome: false),
                     "the user's Focus said only life-safety — honor it")
        XCTAssertEqual(center.level(for: .tamper, awayFromHome: false), .critical,
                       "Focus never silences the smoke alarm")
    }
}

/// A center with its own defaults suite: AlertCenter persists what the user
/// arms, so tests that assign rules must not leave them for the next test.
@MainActor
private func isolatedCenter() -> AlertCenter {
    let suite = "test-alert-center-\(UUID().uuidString)"
    return AlertCenter(defaults: UserDefaults(suiteName: suite) ?? .standard)
}
