//  ResidentWatchTests.swift — what the resident is allowed to call news.
//
//  The resident's whole job is deciding when a household away from home
//  deserves to be woken. Getting that wrong in either direction is a real
//  failure: a missed wake is the feature not working, and a spurious one
//  teaches people to ignore the next. These pin both edges without a network,
//  a clock, or an Apple TV.

import XCTest
@testable import WitnessWall

final class ResidentWatchTests: XCTestCase {

    private func fleet(_ devices: [(String, Bool, String?)]) -> FleetSnapshot {
        FleetSnapshot(
            kernel: "test",
            verifiedThrough: nil,
            devices: devices.map {
                FleetSnapshot.Device(name: $0.0, online: $0.1, chain: $0.2, product: nil)
            }
        )
    }

    func testFirstSightOfAFleetIsNeverNews() {
        // A Wall that boots to a dark Canary is reporting history. Waking the
        // household for it would page them for something that happened before
        // the TV was even watching.
        let dark = fleet([("porch", false, "ok")])
        XCTAssertTrue(ResidentRules.wakes(previous: nil, current: dark).isEmpty)
    }

    func testACanaryGoingDarkIsNews() {
        let before = fleet([("porch", true, "ok")])
        let after = fleet([("porch", false, "ok")])
        XCTAssertEqual(ResidentRules.wakes(previous: before, current: after), [.offline])
    }

    func testAChainThatStopsVerifyingIsNews() {
        let before = fleet([("porch", true, "ok")])
        let after = fleet([("porch", true, "broken")])
        XCTAssertEqual(ResidentRules.wakes(previous: before, current: after), [.integrity])
    }

    func testAnAbsentChainFieldIsNotAnAlarm() {
        // "Not reported" is not "broken" — the same line FleetSnapshot draws.
        let before = fleet([("porch", true, "ok")])
        let after = fleet([("porch", true, nil)])
        XCTAssertTrue(ResidentRules.wakes(previous: before, current: after).isEmpty)
    }

    func testATroubledCanaryStayingTroubledIsNotNewsTwice() {
        let before = fleet([("porch", false, "broken")])
        let after = fleet([("porch", false, "broken")])
        XCTAssertTrue(
            ResidentRules.wakes(previous: before, current: after).isEmpty,
            "a standing problem is not a new event"
        )
    }

    func testANewlyAppearingDarkCanaryIsNotNews() {
        // We do not know when it went dark, and inventing that time is exactly
        // the claim the coarse-time invariant exists to prevent.
        let before = fleet([("porch", true, "ok")])
        let after = fleet([("porch", true, "ok"), ("shed", false, "ok")])
        XCTAssertTrue(ResidentRules.wakes(previous: before, current: after).isEmpty)
    }

    func testRecoveryIsNotAWake() {
        let before = fleet([("porch", false, "ok")])
        let after = fleet([("porch", true, "ok")])
        XCTAssertTrue(
            ResidentRules.wakes(previous: before, current: after).isEmpty,
            "good news does not page a household"
        )
    }

    func testBothKindsOfTroubleAtOnceEarnBoth() {
        let before = fleet([("porch", true, "ok"), ("shed", true, "ok")])
        let after = fleet([("porch", false, "ok"), ("shed", true, "broken")])
        XCTAssertEqual(
            ResidentRules.wakes(previous: before, current: after),
            [.offline, .integrity]
        )
    }

    func testTheRateFloorHoldsPerClass() {
        let t0 = Date(timeIntervalSince1970: 1_700_000_000)
        XCTAssertTrue(ResidentRules.allowed(lastSent: nil, now: t0), "first wake always goes")
        XCTAssertFalse(
            ResidentRules.allowed(lastSent: t0, now: t0.addingTimeInterval(60)),
            "a flapping Canary must not page the household every poll"
        )
        XCTAssertTrue(
            ResidentRules.allowed(
                lastSent: t0, now: t0.addingTimeInterval(ResidentRules.minGapSeconds)),
            "the floor lapses exactly, not approximately"
        )
    }

    @MainActor
    func testOffByDefaultAndPublishesNothingUntilAskedTo() {
        let suite = "resident-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let watch = ResidentWatch(defaults: defaults)
        XCTAssertFalse(watch.isEnabled, "opt-in is the law here too")
        XCTAssertEqual(watch.standing, .off)

        // Observing while off must not even establish a baseline that a later
        // enable could turn into a retroactive wake.
        watch.observe(fleet([("porch", true, "ok")]))
        watch.observe(fleet([("porch", false, "ok")]))
        XCTAssertEqual(watch.standing, .off)
    }

    @MainActor
    func testStandingIsHonestAboutAnUnsignedBuild() {
        // CI builds unsigned, so this is the path CI actually takes: asked to
        // watch, unable to reach iCloud, and saying so rather than implying
        // coverage that does not exist.
        let suite = "resident-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let watch = ResidentWatch(defaults: defaults)
        watch.setEnabled(true)
        if ResidentWatch.cloudUsable {
            // Turning it on starts an ACCOUNT CHECK — it does not start a
            // claim. `.watching` is unreachable until iCloud answers.
            XCTAssertEqual(watch.standing, .checking)
        } else {
            XCTAssertEqual(watch.standing, .unavailable)
            XCTAssertTrue(watch.standing.line.contains("iCloud"))
        }
        XCTAssertFalse(watch.standing.claimsCoverage,
                       "nothing may claim coverage before iCloud has answered")
    }

    @MainActor
    func testEnablingNeverImmediatelyClaimsCoverage() {
        // The bug this pins: `cloudUsable` is a compile-time fact about the
        // BUILD, and on any signed build it is true whether or not the Apple
        // TV is signed into iCloud. Standing watch on that alone told a
        // signed-out household they were covered while every save failed
        // silently — a false assurance in the one place we sell assurance.
        let suite = "resident-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let watch = ResidentWatch(defaults: defaults)
        watch.setEnabled(true)
        XCTAssertFalse(watch.standing.claimsCoverage)
    }

    func testSignedOutAndFailedWakeNeverReadAsCovered() {
        // The two rungs that exist to be believed. If either ever starts
        // claiming coverage, the screen is lying at exactly the moment it
        // matters most.
        XCTAssertFalse(ResidentStanding.signedOut.claimsCoverage)
        XCTAssertFalse(ResidentStanding.wakeFailed.claimsCoverage)
        XCTAssertFalse(ResidentStanding.checking.claimsCoverage)
        XCTAssertTrue(ResidentStanding.watching.claimsCoverage)
        XCTAssertTrue(ResidentStanding.reported.claimsCoverage)
    }

    func testSignedOutSaysWhereToFixIt() {
        // A rung that names the problem without naming the remedy sends the
        // household hunting through tvOS Settings.
        XCTAssertTrue(ResidentStanding.signedOut.line.contains("iCloud"))
        XCTAssertTrue(ResidentStanding.signedOut.line.contains("Users & Accounts"))
        XCTAssertTrue(ResidentStanding.wakeFailed.line.contains("retried"),
                      "a dropped wake must promise the retry it actually does")
    }

    func testEveryStandingSaysSomething() {
        for standing: ResidentStanding in [.off, .unavailable, .signedOut,
                                           .checking, .watching, .reported, .wakeFailed] {
            XCTAssertFalse(standing.line.isEmpty, "a blank line reads as a broken screen")
        }
    }
}
