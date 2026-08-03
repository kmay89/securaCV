// FeedbackPolicyTests.swift
//
// The buzz discipline as executable rules. Every test names the promise it
// guards; the promises are what keep this a security app someone leaves ON.

import XCTest
@testable import SecuraCV

final class FeedbackPolicyTests: XCTestCase {
    func testAnOrdinaryWeekProducesZeroHaptics() {
        // Every transition an ordinary week contains stays silent.
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .ok, to: .ok))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .ok, to: .notice))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .notice, to: .ok))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .notice, to: .warn))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .warn, to: .notice))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .warn, to: .warn))
    }

    func testEscalationIsFeltOnceAtTheCrossing() {
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .ok, to: .alert),
                       .fleetEscalated(to: .alert))
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .notice, to: .tamper),
                       .fleetEscalated(to: .tamper))
        // Worsening within alarm territory is still an escalation…
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .alert, to: .tamper),
                       .fleetEscalated(to: .tamper))
        // …but the cycles that merely STAY there are silent.
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .alert, to: .alert))
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .tamper, to: .tamper))
    }

    func testTheAllClearFiresOnLeavingAlarmTerritoryByAnyPath() {
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .alert, to: .ok), .allClear)
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .tamper, to: .notice), .allClear)
        // Settling through "needs a look" still delivers closure.
        XCTAssertEqual(FeedbackPolicy.fleetTransition(from: .alert, to: .warn), .allClear)
        // And de-escalating within alarm territory is NOT closure.
        XCTAssertNil(FeedbackPolicy.fleetTransition(from: .tamper, to: .alert))
    }

    func testAPathTestAnswersInTheHandThatAsked() {
        XCTAssertEqual(FeedbackPolicy.pathTest(verified: true), .pathVerified)
        XCTAssertEqual(FeedbackPolicy.pathTest(verified: false), .pathFailed)
    }
}
