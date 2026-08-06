// AwayReachTests.swift — the two rules that decide what an away phone says.
//
// Both existed as intentions before they existed as behavior: `reach` was
// honored by AlertCenter but the caller passed a literal `false`, so the
// "On Wi-Fi only" setting never suppressed anything in production, and a
// phone driving away reported every Canary as dark because it could no
// longer hear them. These pin the fixed versions.

import XCTest
@testable import SecuraCV

final class AwayReachTests: XCTestCase {

    @MainActor
    func testOnWiFiOnlyActuallySuppressesWhenAway() {
        let center = AlertCenter()
        center.rules = [
            AlertRule(id: "home-only", title: "Everyday activity",
                      minSeverity: .notice, reach: .onWiFiOnly, enabled: true)
        ]
        XCTAssertNotNil(
            center.level(for: .notice, awayFromHome: false),
            "at home, an on-Wi-Fi rule speaks")
        XCTAssertNil(
            center.level(for: .notice, awayFromHome: true),
            "away, it must stay quiet — this is the branch that never ran")
    }

    @MainActor
    func testAnywhereRulesStillTravel() {
        let center = AlertCenter()
        center.rules = [
            AlertRule(id: "anywhere", title: "Tamper",
                      minSeverity: .alert, reach: .anywhere, enabled: true)
        ]
        XCTAssertNotNil(center.level(for: .alert, awayFromHome: true))
    }

    func testAwayPhoneCannotClaimDarkness() {
        // Silence from across town is ambiguous: the Canary may be dead, or
        // the phone may simply have left. Claiming the first is the drive-to-
        // work notification storm.
        XCTAssertTrue(
            AlertCenter.unknowableFromAway(awayFromHome: true, isDark: true, tamper: false),
            "away + dark + no tamper is exactly the unknowable case")
        XCTAssertFalse(
            AlertCenter.unknowableFromAway(awayFromHome: false, isDark: true, tamper: false),
            "at home, darkness is a real observation and must be reported")
    }

    func testTamperSurvivesTheDistance() {
        // The device said so itself before going quiet — distance does not
        // make that claim less true, and it is the one that matters most.
        XCTAssertFalse(
            AlertCenter.unknowableFromAway(awayFromHome: true, isDark: true, tamper: true),
            "a tampered Canary reports from anywhere, dark or not")
    }

    func testALiveWitnessIsNeverSuppressed() {
        for away in [true, false] {
            XCTAssertFalse(
                AlertCenter.unknowableFromAway(awayFromHome: away, isDark: false, tamper: false),
                "only darkness is unknowable from away; everything else speaks")
        }
    }
}
