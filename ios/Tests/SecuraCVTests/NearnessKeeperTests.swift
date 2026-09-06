// NearnessKeeperTests.swift
//
// The ambient nearness claim, held still: the first fresh voice takes the
// title outright, an incumbent keeps it until the SAME challenger leads by
// the margin for the full dwell, a broken streak starts over, a returning
// beacon starts a fresh average instead of resurrecting a stale one, a
// quiet beacon abdicates, and silence from everyone means nobody wears the
// badge — absence is the honest default.

import XCTest
@testable import SecuraCV

final class NearnessKeeperTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    func testTheFirstFreshVoiceTakesTheTitleOutright() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        XCTAssertEqual(keeper.evaluate(at: t0), "porch")
    }

    func testAnIncumbentSurvivesASubMarginChallenger() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        _ = keeper.evaluate(at: t0)
        // 5 dB stronger — inside the noise bar, for as many rounds as it
        // likes: ordinary churn between two rooms must never hop the badge.
        for i in 1...5 {
            let now = t0.addingTimeInterval(Double(i))
            keeper.observe(id: "porch", rssiDBM: -70, at: now)
            keeper.observe(id: "kitchen", rssiDBM: -65, at: now)
            XCTAssertEqual(keeper.evaluate(at: now), "porch")
        }
    }

    func testADecisiveChallengerNeedsTheFullDwell() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        _ = keeper.evaluate(at: t0)
        // Round 1 of a clear lead: still the incumbent's title.
        let t1 = t0.addingTimeInterval(1)
        keeper.observe(id: "porch", rssiDBM: -70, at: t1)
        keeper.observe(id: "kitchen", rssiDBM: -50, at: t1)
        XCTAssertEqual(keeper.evaluate(at: t1), "porch")
        // Round 2 of the SAME lead: the move is real — title changes.
        let t2 = t0.addingTimeInterval(2)
        keeper.observe(id: "porch", rssiDBM: -70, at: t2)
        keeper.observe(id: "kitchen", rssiDBM: -50, at: t2)
        XCTAssertEqual(keeper.evaluate(at: t2), "kitchen")
    }

    func testABrokenStreakStartsOver() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        _ = keeper.evaluate(at: t0)
        // Kitchen leads once…
        let t1 = t0.addingTimeInterval(1)
        keeper.observe(id: "porch", rssiDBM: -70, at: t1)
        keeper.observe(id: "kitchen", rssiDBM: -50, at: t1)
        XCTAssertEqual(keeper.evaluate(at: t1), "porch")
        // …then collapses hard enough that even the smoothing's memory of
        // the strong round lands back inside the margin: streak spent.
        let t2 = t0.addingTimeInterval(2)
        keeper.observe(id: "porch", rssiDBM: -70, at: t2)
        keeper.observe(id: "kitchen", rssiDBM: -90, at: t2)
        XCTAssertEqual(keeper.evaluate(at: t2), "porch")
        // Leading again counts as round ONE, not round two.
        let t3 = t0.addingTimeInterval(3)
        keeper.observe(id: "porch", rssiDBM: -70, at: t3)
        keeper.observe(id: "kitchen", rssiDBM: -50, at: t3)
        XCTAssertEqual(keeper.evaluate(at: t3), "porch")
    }

    func testAReturningBeaconStartsAFreshAverageNotAResurrectedOne() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -40, at: t0)
        _ = keeper.evaluate(at: t0)
        // Twenty silent seconds bury that loud -40. Porch returns faint
        // beside a genuinely near peer: with the history honestly reset the
        // peer's decisive lead wins after the normal dwell — a resurrected
        // average (-60 dBm of stale memory) would have denied the challenge
        // forever, since -65 never clears -60 by the margin.
        for round in 1...NearnessKeeper.dwellEvaluations {
            let now = t0.addingTimeInterval(20 + Double(round))
            keeper.observe(id: "porch", rssiDBM: -90, at: now)
            keeper.observe(id: "kitchen", rssiDBM: -65, at: now)
            let expected = round < NearnessKeeper.dwellEvaluations ? "porch" : "kitchen"
            XCTAssertEqual(keeper.evaluate(at: now), expected)
        }
    }

    func testAQuietIncumbentAbdicates() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        _ = keeper.evaluate(at: t0)
        // Ten silent seconds later the claim is gone — and with another
        // fresh voice around, THAT one takes the vacant title outright.
        let later = t0.addingTimeInterval(10)
        keeper.observe(id: "kitchen", rssiDBM: -80, at: later)
        XCTAssertEqual(keeper.evaluate(at: later), "kitchen")
    }

    func testSilenceFromEveryoneMeansNobodyWearsTheBadge() {
        var keeper = NearnessKeeper()
        keeper.observe(id: "porch", rssiDBM: -70, at: t0)
        _ = keeper.evaluate(at: t0)
        XCTAssertNil(keeper.evaluate(at: t0.addingTimeInterval(60)))
    }
}
