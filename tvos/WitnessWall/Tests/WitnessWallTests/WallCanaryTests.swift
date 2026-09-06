//  WallCanaryTests.swift — the wall's fold into the mood engine, pinned.
//
//  The engine's math is pinned on the phone (CanaryMoodTests mirrors
//  bird_mood.h); what THIS surface owns is the fold from wall truth to
//  engine inputs and the one ambient sentence — so that is what these pin.
//  The load-bearing rules: a field the Wall cannot name stays at its honest
//  zero, "verified" feeds the bird only when this TV walked the chain
//  itself, chain trouble is the alarm that hides the bird entirely, and
//  the sentence never words an alarm.

import XCTest
import UIKit
@testable import WitnessWall

final class WallCanaryTests: XCTestCase {
    private func fleet(_ devices: [(String, Bool, String?)],
                       hub: [String?]? = nil) -> FleetSnapshot {
        FleetSnapshot(
            kernel: "test",
            verifiedThrough: nil,
            devices: devices.enumerated().map { index, d in
                var device = FleetSnapshot.Device(name: d.0, online: d.1,
                                                  chain: d.2, product: nil)
                if let hub, index < hub.count { device.hub = hub[index] }
                return device
            }
        )
    }

    private func report(ok: Bool) -> VerifyReport {
        VerifyReport(ok: ok, verified: 12, head: "abc", failedAt: nil,
                     kind: nil, detail: nil,
                     message: ok ? "chain ok" : "chain broke")
    }

    // MARK: - the fold (WallCanary.inputs)

    func testDarkDevicesAreLostAndTheWallKnowsNoLate() {
        let i = WallCanary.inputs(
            fleet: fleet([("porch", false, "ok"), ("hall", true, "ok"),
                          ("shed", false, nil)]),
            wallDown: false, report: nil)
        XCTAssertEqual(i.lostWitnesses, 2)
        XCTAssertEqual(i.staleWitnesses, 0,
                       "no grace ladder on this surface — dark, never late")
    }

    func testHonestZerosStayZero() {
        // Fields the Wall cannot name must never be invented.
        let i = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                  wallDown: false, report: report(ok: true))
        XCTAssertFalse(i.hubFlapping)
        XCTAssertEqual(i.unackedOld, 0)
        XCTAssertFalse(i.night)
    }

    func testLinksDownFromAHubOrFromTheWallItself() {
        let hubDown = WallCanary.inputs(
            fleet: fleet([("porch", true, "ok")], hub: ["down"]),
            wallDown: false, report: nil)
        XCTAssertTrue(hubDown.linksDown)

        let wallDown = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                         wallDown: true, report: nil)
        XCTAssertTrue(wallDown.linksDown)

        let fine = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                     wallDown: false, report: nil)
        XCTAssertFalse(fine.linksDown)
    }

    func testVerifiedFeedsTheBirdOnlyWhenThisTVWalkedTheChain() {
        let selfReport = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                           wallDown: false, report: nil)
        XCTAssertFalse(selfReport.allVerified,
                       "a device's self-stamp is its own word, not a verdict")

        let walked = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                       wallDown: false, report: report(ok: true))
        XCTAssertTrue(walked.allVerified)

        let walkedButDark = WallCanary.inputs(
            fleet: fleet([("porch", false, "ok")]),
            wallDown: false, report: report(ok: true))
        XCTAssertFalse(walkedButDark.allVerified,
                       "verified means everyone answered, too")

        let walkedButNobody = WallCanary.inputs(
            fleet: fleet([]), wallDown: false, report: report(ok: true))
        XCTAssertFalse(walkedButNobody.allVerified,
                       "an empty fleet proved nothing")
    }

    func testAVerifiedPassAndALiveAlarmAreNeverBothClaimed() {
        // A sealed log this TV walked can verify while a device still
        // reports its own chain troubled — the alarm outranks the pass.
        let i = WallCanary.inputs(
            fleet: fleet([("porch", true, "ok"), ("shed", true, "tampered")]),
            wallDown: false, report: report(ok: true))
        XCTAssertTrue(i.alarmUnacked)
        XCTAssertFalse(i.allVerified,
                       "a fully-verified pass cannot coexist with a live alarm")
    }

    func testTheWallsOwnOutageIsALinkProblemAndNothingMore() {
        // The degrade fold's whole honesty, pinned as an equality: linksDown
        // and not one invented field on top.
        var expected = CanaryMoodInputs()
        expected.linksDown = true
        XCTAssertEqual(
            WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                              wallDown: true, report: nil),
            expected)
    }

    func testChainTroubleIsTheAlarmThatHidesTheBird() {
        // Either verdict — this TV's own walk failing, or a device saying
        // its record didn't verify — hands the stage to the banners.
        let deviceSays = WallCanary.inputs(
            fleet: fleet([("porch", true, "tampered")]),
            wallDown: false, report: nil)
        XCTAssertTrue(deviceSays.alarmUnacked)

        let tvSays = WallCanary.inputs(fleet: fleet([("porch", true, "ok")]),
                                       wallDown: false, report: report(ok: false))
        XCTAssertTrue(tvSays.alarmUnacked)

        var s = CanaryMoodState()
        CanaryMoodEngine.minute(&s, tvSays)
        XCTAssertEqual(CanaryMoodEngine.face(s, tvSays), .hidden,
                       "never cute during a real alarm")
        XCTAssertFalse(s.dayClean, "an alarm day is not a clean day")
    }

    func testAnAbsentOrUnknownChainClaimIsNotAnAlarm() {
        // Mirrors chainIsTroubled: an absent claim is not a broken chain.
        let i = WallCanary.inputs(
            fleet: fleet([("display", true, nil), ("hall", true, "unknown")]),
            wallDown: false, report: nil)
        XCTAssertFalse(i.alarmUnacked)
    }

    // MARK: - the sentence (WallCanary.line)

    private func line(face: CanaryFace, posture: CanaryPosture = .asFace,
                      trustDays: Int = 0, milestone: Bool = false,
                      fleet snapshot: FleetSnapshot? = nil) -> String? {
        var s = CanaryMoodState()
        s.trustDays = trustDays
        return WallCanary.line(face: face, posture: posture, state: s,
                               milestone: milestone,
                               fleet: snapshot ?? fleet([("porch", true, "ok")]))
    }

    func testTheSentenceNeverWordsAnAlarm() {
        XCTAssertNil(line(face: .hidden))
        XCTAssertNil(line(face: .asleep))
    }

    func testCalmSpeaksTrustHonestly() {
        XCTAssertEqual(line(face: .calm), "Watching with you")
        XCTAssertEqual(line(face: .calm, trustDays: 9), "9 clean days together")
        XCTAssertEqual(line(face: .calm, trustDays: 7, milestone: true),
                       "A clean week together")
        XCTAssertEqual(line(face: .calm, trustDays: 30, milestone: true),
                       "A clean month together")
    }

    func testCallingNamesTheDarkCanary() {
        let dark = fleet([("hall", true, "ok"), ("porch", false, "ok")])
        XCTAssertEqual(line(face: .worried, posture: .calling, fleet: dark),
                       "Calling for porch")
    }

    func testTheRestOfTheLadderSpeaksTheSharedVoice() {
        XCTAssertEqual(line(face: .worried, posture: .asFace),
                       "Something feels off")
        XCTAssertEqual(line(face: .distressed),
                       "Feeling rough — the fleet needs care")
    }

    // MARK: - the character's body ships in this bundle

    func testTheCanaryImagesetIsInTheTVBundle() {
        // Image("Canary") compiles green with no asset and renders blank —
        // the one regression no build gate can see. Same protection the
        // iPhone bundle has (BundleCharmTests).
        XCTAssertNotNil(UIImage(named: "Canary"),
                        "the Canary imageset must ship in the tvOS bundle")
    }
}
