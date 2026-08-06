// CoverageTests.swift — pinning the answer to "would I be told?".
//
// The value of this screen is entirely in its honesty, so the tests are
// mostly about what it must REFUSE to say: no cheerful count when nothing
// can deliver, no claimed coverage for a lane this device cannot observe,
// and no lane surviving the one failure that blinds all of them (iOS
// notifications turned off). Every rung carries copy the user reads, so a
// broken lane asserting a non-empty explanation is a real assertion, not
// ceremony.

import XCTest
@testable import SecuraCV

final class CoverageTests: XCTestCase {

    /// The shape a brand-new install is actually in.
    private func fresh(
        notificationsAuthorized: Bool = true,
        anyRuleArmed: Bool = true,
        awayReachReady: Bool = true,
        awayReachExplanation: String = "iCloud is not signed in.",
        homeKitEnabled: Bool = true,
        homeKitHubPresent: Bool = true,
        residentKnown: Bool = false
    ) -> Coverage {
        Coverage.evaluate(
            notificationsAuthorized: notificationsAuthorized,
            anyRuleArmed: anyRuleArmed,
            awayReachReady: awayReachReady,
            awayReachExplanation: awayReachExplanation,
            homeKitEnabled: homeKitEnabled,
            homeKitHubPresent: homeKitHubPresent,
            residentKnown: residentKnown)
    }

    private func lane(_ c: Coverage, _ id: String) -> CoverageLane {
        guard let l = c.lanes.first(where: { $0.id == id }) else {
            XCTFail("no lane \(id)"); return c.lanes[0]
        }
        return l
    }

    func testEveryLaneIsAccountedFor() {
        // A lane that silently disappears would read as "nothing more to
        // check" — the failure mode this screen exists to end.
        XCTAssertEqual(fresh().lanes.map(\.id),
                       ["local", "away", "resident", "applehome", "relay"])
    }

    func testNothingWorkingSaysSoPlainly() {
        let c = fresh(notificationsAuthorized: false,
                      awayReachReady: false,
                      homeKitEnabled: false)
        XCTAssertEqual(c.workingCount, 0)
        XCTAssertEqual(c.headline, "Nothing would reach you")
        XCTAssertTrue(c.summary.contains("could not tell you"),
                      "the zero case must state the consequence, not a status")
    }

    func testOneStandingPathIsNotCalledSafe() {
        // Redundancy is the design. A single path is working, and the copy
        // has to say what it costs — not congratulate.
        let c = fresh(awayReachReady: false, homeKitEnabled: false)
        XCTAssertEqual(c.workingCount, 1)
        XCTAssertEqual(c.headline, "One way to reach you")
        XCTAssertTrue(c.summary.contains("nothing is behind it"))
    }

    func testNotificationsOffBreaksTheLocalLane() {
        let c = fresh(notificationsAuthorized: false)
        guard case .broken(let why) = lane(c, "local").standing else {
            return XCTFail("notifications off is broken, not merely off")
        }
        XCTAssertTrue(why.contains("Settings"), "must name where to fix it")
    }

    func testNoArmedRuleIsOffRatherThanBroken() {
        // Nobody turned it on is a different sentence from it is failing,
        // and pointing at Settings here would send the user somewhere with
        // nothing to fix.
        let c = fresh(anyRuleArmed: false)
        guard case .off = lane(c, "local").standing else {
            return XCTFail("an unarmed rule set is off, not broken")
        }
    }

    func testAwayLaneCarriesTheRealReason() {
        // The explanation comes from AwayPush, not from a second copy of the
        // reasoning here — this asserts it is passed through verbatim.
        let c = fresh(awayReachReady: false,
                      awayReachExplanation: "iCloud is not signed in.")
        guard case .broken(let why) = lane(c, "away").standing else {
            return XCTFail("an unready away path is broken")
        }
        XCTAssertEqual(why, "iCloud is not signed in.")
    }

    func testResidentIsNeverClaimedFromThePhone() {
        // Whether an Apple TV stands watch is a fact about that Apple TV.
        // Guessing either way here is the exact dishonesty this screen ends.
        guard case .unobservable = lane(fresh(), "resident").standing else {
            return XCTFail("the phone cannot see another device's setting")
        }
        XCTAssertFalse(lane(fresh(), "resident").standing.isCovered)
    }

    func testResidentCountsOnceItIsActuallyKnown() {
        let c = fresh(residentKnown: true)
        XCTAssertTrue(lane(c, "resident").standing.isCovered)
    }

    func testAppleHomeWithoutAHubIsBrokenNotOff() {
        // Publishing with no HomePod or Apple TV looks configured and
        // delivers nothing away from home — the worst kind of quiet.
        let c = fresh(homeKitHubPresent: false)
        guard case .broken(let why) = lane(c, "applehome").standing else {
            return XCTFail("no home hub is a real break")
        }
        XCTAssertTrue(why.contains("HomePod") || why.contains("Apple TV"))
    }

    func testAppleHomeOffIsOff() {
        guard case .off = lane(fresh(homeKitEnabled: false), "applehome").standing else {
            return XCTFail("opt-in and not opted in is off")
        }
    }

    func testRelayLaneNeverClaimsCoverageAndNamesTheDrill() {
        // It runs on the hub. The only honest proof is the one that sends a
        // real notification, so the copy has to hand over that command.
        for known in [true, false] {
            let c = fresh(residentKnown: known)
            guard case .unobservable(let why) = lane(c, "relay").standing else {
                return XCTFail("the phone cannot check the hub's relay")
            }
            XCTAssertTrue(why.contains("--send-test"),
                          "must name the drill that actually proves it")
        }
    }

    func testUnobservableLanesNeverInflateTheCount() {
        // Two of five lanes are unobservable from a phone, so a fully
        // healthy phone tops out at three. If this ever reads five, the
        // model started counting things it cannot see.
        XCTAssertEqual(fresh().workingCount, 3)
        XCTAssertEqual(fresh().headline, "3 ways to reach you")
    }

    func testEveryLaneSaysWhatItCarries() {
        // "Which paths are down" is only useful next to "and what did they
        // cover" — an empty string here would make the screen unreadable.
        for l in fresh().lanes {
            XCTAssertFalse(l.name.isEmpty, "\(l.id) has no name")
            XCTAssertFalse(l.carries.isEmpty, "\(l.id) says nothing about its scope")
        }
    }

    func testEveryNonCoveredLaneExplainsItself() {
        let c = fresh(notificationsAuthorized: false,
                      awayReachReady: false,
                      awayReachExplanation: "iCloud is not signed in.",
                      homeKitEnabled: false)
        for l in c.lanes where !l.standing.isCovered {
            switch l.standing {
            case .covered:
                continue
            case .broken(let why), .off(let why), .unobservable(let why):
                XCTAssertFalse(why.isEmpty, "\(l.id) fails without saying why")
            }
        }
    }

    func testEvaluateIsTotalAcrossEveryInput() {
        // Pure and total is the claim in the header; this walks all 64
        // combinations and asserts the model never drops or duplicates a
        // lane on any of them.
        for bits in 0..<64 {
            let c = Coverage.evaluate(
                notificationsAuthorized: bits & 1 != 0,
                anyRuleArmed: bits & 2 != 0,
                awayReachReady: bits & 4 != 0,
                awayReachExplanation: "…",
                homeKitEnabled: bits & 8 != 0,
                homeKitHubPresent: bits & 16 != 0,
                residentKnown: bits & 32 != 0)
            XCTAssertEqual(c.lanes.count, 5)
            XCTAssertEqual(Set(c.lanes.map(\.id)).count, 5)
            XCTAssertFalse(c.headline.isEmpty)
            XCTAssertFalse(c.summary.isEmpty)
            XCTAssertTrue((0...5).contains(c.workingCount))
        }
    }
}
