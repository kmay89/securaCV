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
        homeKitAutomationCount: Int = 1,
        residentKnown: Bool = false
    ) -> Coverage {
        Coverage.evaluate(
            notificationsAuthorized: notificationsAuthorized,
            anyRuleArmed: anyRuleArmed,
            awayReachReady: awayReachReady,
            awayReachExplanation: awayReachExplanation,
            homeKitEnabled: homeKitEnabled,
            homeKitHubPresent: homeKitHubPresent,
            homeKitAutomationCount: homeKitAutomationCount,
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

    // MARK: - a path needs both ends

    func testAwayIsNotCountedOnAProvenReceiverAlone() {
        // `awayReachReady` proves the CloudKit subscription that RECEIVES a
        // wake exists. Something at home still has to SEND one, and this
        // model says outright that it cannot see whether anything does.
        // Counting the receiver as a working path inflated the headline in
        // exactly the away-with-nobody-home case the screen exists to expose.
        let c = fresh(awayReachReady: true, residentKnown: false)
        guard case .unobservable = lane(c, "away").standing else {
            return XCTFail("half a path is not a path")
        }
    }

    func testAwayCountsOnceBothEndsAreKnown() {
        let c = fresh(awayReachReady: true, residentKnown: true)
        XCTAssertTrue(lane(c, "away").standing.isCovered)
    }

    func testAwayInheritsTheNotificationFloor() {
        // A wake that arrives at a phone with notifications denied is a row
        // in a database, not an alert.
        let c = fresh(notificationsAuthorized: false, awayReachReady: true, residentKnown: true)
        guard case .broken = lane(c, "away").standing else {
            return XCTFail("no notifications means no away alert either")
        }
    }

    func testAppleHomeWithoutAnAutomationTellsNobody() {
        // Publishing to Home is not being told BY Home: an accessory nobody
        // wrote an automation against changes its characteristic quietly
        // forever. Enabled + a hub is the setup being complete, not the path
        // working.
        let c = fresh(homeKitAutomationCount: 0)
        guard case .unobservable(let why) = lane(c, "applehome").standing else {
            return XCTFail("no automation means Apple Home would tell nobody")
        }
        XCTAssertTrue(why.contains("Apple Home"), "must say where to add one")
    }

    func testAppleHomeCountsWhenAnAutomationOfOursExists() {
        // Ours are UUID-anchored, so this is an observation and not a guess.
        XCTAssertTrue(lane(fresh(homeKitAutomationCount: 2), "applehome").standing.isCovered)
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
        // A phone with everything IT can control set correctly still tops out
        // at two, because three of the five lanes depend on something this
        // device cannot observe — the Apple TV, the hub's relay, and (through
        // the resident) the away path's sending end. If this number ever
        // climbs without those facts arriving, the model started counting
        // things it cannot see, which is the whole failure this screen exists
        // to end.
        XCTAssertEqual(fresh().workingCount, 2)
        XCTAssertEqual(fresh().headline, "2 ways to reach you")

        // And with the resident known, the away lane's other end is proven.
        XCTAssertEqual(fresh(residentKnown: true).workingCount, 4)
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
        // Pure and total is the claim in the header; this walks all 128
        // combinations and asserts the model never drops or duplicates a
        // lane on any of them — and never counts a lane it cannot observe.
        for bits in 0..<128 {
            let notificationsAuthorized = bits & 1 != 0
            let residentKnown = bits & 32 != 0
            let c = Coverage.evaluate(
                notificationsAuthorized: notificationsAuthorized,
                anyRuleArmed: bits & 2 != 0,
                awayReachReady: bits & 4 != 0,
                awayReachExplanation: "…",
                homeKitEnabled: bits & 8 != 0,
                homeKitHubPresent: bits & 16 != 0,
                homeKitAutomationCount: (bits & 64 != 0) ? 1 : 0,
                residentKnown: residentKnown)
            XCTAssertEqual(c.lanes.count, 5)
            XCTAssertEqual(Set(c.lanes.map(\.id)).count, 5)
            XCTAssertFalse(c.headline.isEmpty)
            XCTAssertFalse(c.summary.isEmpty)
            XCTAssertTrue((0...5).contains(c.workingCount))

            // The relay is never observable from a phone, on any input.
            XCTAssertFalse(lane(c, "relay").standing.isCovered)
            // Notifications are the floor under every lane that lands on this
            // device: with them off, neither the local nor the away lane may
            // ever read as working.
            if !notificationsAuthorized {
                XCTAssertFalse(lane(c, "local").standing.isCovered)
                XCTAssertFalse(lane(c, "away").standing.isCovered)
            }
            // The away path needs a sender. Without a known resident it may
            // never count, however healthy this phone's own half is.
            if !residentKnown {
                XCTAssertFalse(lane(c, "away").standing.isCovered)
                XCTAssertFalse(lane(c, "resident").standing.isCovered)
            }
        }
    }
}
