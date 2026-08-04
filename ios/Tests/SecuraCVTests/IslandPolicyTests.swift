// IslandPolicyTests.swift
//
// The island is an episode, not wallpaper — the promise that an ordinary
// quiet day puts NOTHING in the status bar, pinned the same way
// FeedbackPolicyTests pins "an ordinary week produces zero haptics."

import XCTest
@testable import SecuraCV

final class IslandPolicyTests: XCTestCase {
    func testAnOrdinaryQuietDayPutsNothingInTheStatusBar() {
        XCTAssertFalse(IslandPolicy.shouldShow(worstSeverity: .ok, heartbeat: .alive))
        XCTAssertFalse(IslandPolicy.shouldShow(worstSeverity: .ok, heartbeat: .unknown))
    }

    func testRoutineActivityIsNotAnEpisode() {
        // Notice-level comings and goings live on pull surfaces (Today, the
        // widgets) — they never occupy the island.
        XCTAssertFalse(IslandPolicy.shouldShow(worstSeverity: .notice, heartbeat: .alive))
    }

    func testAnythingNeedingAHumanIsAnEpisode() {
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .warn, heartbeat: .alive))
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .alert, heartbeat: .alive))
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .tamper, heartbeat: .alive))
    }

    func testTheDeadMansSwitchTalkingIsAnEpisodeEvenWhenRowsLookGreen() {
        // A fleet that LOOKS fine while the delivery path is dark is exactly
        // the false comfort the island must interrupt.
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .ok, heartbeat: .dark))
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .ok, heartbeat: .failed))
    }

    func testAPathTestInFlightIsAnEpisodeTheUserStarted() {
        XCTAssertTrue(IslandPolicy.shouldShow(worstSeverity: .ok, heartbeat: .testing))
    }

    func testTheAllClearLingersBrieflyAndNeverBecomesFurniture() {
        XCTAssertGreaterThan(IslandPolicy.allClearLinger, 0)
        XCTAssertLessThanOrEqual(IslandPolicy.allClearLinger, 5 * 60)
    }
}
