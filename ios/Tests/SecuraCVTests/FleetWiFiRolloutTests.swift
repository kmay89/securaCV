// FleetWiFiRolloutTests.swift
//
// The staged Wi-Fi rollout's policy, held still: the pilot is the healthiest
// reachable Canary, the fleet is never touched before the pilot proves the
// password, the display family is named hands-on rather than pretended at,
// and the credential bounds mirror the firmware's own.

import XCTest
@testable import SecuraCV

final class FleetWiFiRolloutTests: XCTestCase {
    private func candidate(_ id: String,
                           updatable: Bool = true,
                           online: Bool = true,
                           ble: Bool = false,
                           rssi: Int? = nil,
                           pinned: Bool = false) -> FleetWiFiRollout.Candidate {
        FleetWiFiRollout.Candidate(id: id, name: id, updatable: updatable,
                                   online: online, bleReachable: ble, rssiDBM: rssi,
                                   tlsPinned: pinned)
    }

    // MARK: - lanes

    func testAnOnlineWAPWithoutAPinRidesCleartextHTTP() {
        // Plain http still works — but it is named as what it is, and the
        // plan will ask before the password crosses the LAN in the clear.
        XCTAssertEqual(candidate("a").path, .httpCleartext)
        XCTAssertTrue(candidate("a").path.isHTTP)
    }

    func testAPinnedHTTPSWAPRidesEncryptedHTTP() {
        XCTAssertEqual(candidate("a", pinned: true).path, .http)
        XCTAssertTrue(candidate("a", pinned: true).path.isHTTP)
    }

    func testBondedBLEOutranksCleartextEvenWhenOnline() {
        // Online but unpinned, and in Bluetooth range: the bonded lane wins
        // over the one that would leak the password (roadmap row 13).
        XCTAssertEqual(candidate("a", ble: true).path, .ble)
    }

    func testPinnedHTTPSOutranksBLEWhenOnline() {
        // Both lanes are encrypted; https proves the password in seconds.
        XCTAssertEqual(candidate("a", ble: true, pinned: true).path, .http)
    }

    func testAPinIsNoUseToADarkDevice() {
        XCTAssertEqual(candidate("a", online: false, ble: true, pinned: true).path, .ble)
        XCTAssertEqual(candidate("a", online: false, pinned: true).path, .unreachable)
    }

    func testADarkWAPInRangeRidesTheBLERescue() {
        XCTAssertEqual(candidate("a", online: false, ble: true).path, .ble)
    }

    func testADisplayIsHandsOn() {
        XCTAssertEqual(candidate("d", updatable: false).path, .handsOn)
    }

    func testADarkOutOfRangeWAPIsUnreachable() {
        XCTAssertEqual(candidate("a", online: false).path, .unreachable)
    }

    // MARK: - the plan

    func testThePilotIsTheStrongestSignal() {
        let plan = FleetWiFiRollout.plan([
            candidate("weak", rssi: -80),
            candidate("strong", rssi: -40),
            candidate("mid", rssi: -60),
        ])
        XCTAssertEqual(plan.pilot?.id, "strong")
        XCTAssertEqual(plan.followers.map(\.id), ["mid", "weak"])
    }

    func testHTTPOutranksBLEForThePilot() {
        // The pilot's job is a FAST proof; HTTP answers in seconds while a
        // BLE bond ceremony can take a minute.
        let plan = FleetWiFiRollout.plan([
            candidate("ble", online: false, ble: true, rssi: -30),
            candidate("http", rssi: -70),
        ])
        XCTAssertEqual(plan.pilot?.id, "http")
        XCTAssertEqual(plan.followers.map(\.id), ["ble"])
    }

    func testAPinnedPilotOutranksACleartextOne() {
        // If the proof can be made without putting the password in the
        // clear, it is — whatever the signal strengths say.
        let plan = FleetWiFiRollout.plan([
            candidate("plain", rssi: -30),
            candidate("pinned", rssi: -80, pinned: true),
        ])
        XCTAssertEqual(plan.pilot?.id, "pinned")
        XCTAssertEqual(plan.followers.map(\.id), ["plain"])
    }

    func testLanesKeepTheirOrderAmongFollowers() {
        let plan = FleetWiFiRollout.plan([
            candidate("ble", online: false, ble: true, rssi: -30),
            candidate("plain", rssi: -40),
            candidate("pinned-weak", rssi: -85, pinned: true),
            candidate("pinned-strong", rssi: -45, pinned: true),
        ])
        XCTAssertEqual(plan.pilot?.id, "pinned-strong")
        XCTAssertEqual(plan.followers.map(\.id), ["pinned-weak", "plain", "ble"])
    }

    // MARK: - the cleartext disclosure

    func testAPlanWithACleartextTargetNeedsTheDisclosure() {
        let plan = FleetWiFiRollout.plan([candidate("pinned", pinned: true), candidate("plain")])
        XCTAssertTrue(plan.needsCleartextDisclosure)
        XCTAssertEqual(plan.cleartextTargets.map(\.id), ["plain"])
    }

    func testAnAllEncryptedPlanNeedsNoDisclosure() {
        let plan = FleetWiFiRollout.plan([
            candidate("pinned", pinned: true),
            candidate("ble", online: false, ble: true),
            candidate("display", updatable: false),
        ])
        XCTAssertFalse(plan.needsCleartextDisclosure)
        XCTAssertTrue(plan.cleartextTargets.isEmpty)
    }

    func testACleartextPilotAloneStillNeedsTheDisclosure() {
        let plan = FleetWiFiRollout.plan([candidate("only")])
        XCTAssertEqual(plan.pilot?.path, .httpCleartext)
        XCTAssertTrue(plan.needsCleartextDisclosure)
    }

    func testDecliningCleartextReStagesAroundAnEncryptedPilot() {
        // One plain-http Canary the user did not approve must not hold the
        // pinned and bonded ones: it is set aside, and the pilot is re-picked
        // from the encrypted lanes in the same lane-then-strength order.
        let plan = FleetWiFiRollout.plan([candidate("clear", rssi: -40),
                                          candidate("pinned", rssi: -70, pinned: true),
                                          candidate("dark", online: false, ble: true)])
        XCTAssertEqual(plan.pilot?.id, "pinned")
        let (kept, declined) = plan.excludingCleartext()
        XCTAssertEqual(declined.map(\.id), ["clear"])
        XCTAssertEqual(kept.pilot?.id, "pinned")
        XCTAssertEqual(kept.followers.map(\.id), ["dark"])
        XCTAssertFalse(kept.needsCleartextDisclosure)
        XCTAssertFalse(plan.allPushesAreCleartext)
    }

    func testAnAllCleartextPlanHasNothingToRunUntilApproved() {
        let plan = FleetWiFiRollout.plan([candidate("a"), candidate("b")])
        XCTAssertTrue(plan.allPushesAreCleartext)
        let (kept, declined) = plan.excludingCleartext()
        XCTAssertNil(kept.pilot)
        XCTAssertTrue(kept.followers.isEmpty)
        XCTAssertEqual(declined.count, 2)
    }

    func testOnlyCleartextWaitsOnTheAcknowledgment() {
        XCTAssertFalse(FleetWiFiRollout.mayPush(.httpCleartext, cleartextApproved: false))
        XCTAssertTrue(FleetWiFiRollout.mayPush(.httpCleartext, cleartextApproved: true))
        for lane in [FleetWiFiRollout.Path.http, .ble, .handsOn, .unreachable] {
            XCTAssertTrue(FleetWiFiRollout.mayPush(lane, cleartextApproved: false), "\(lane)")
        }
    }

    func testTheDisclosureSaysWhatCrossesTheNetwork() {
        // Copy is contract here: the user must be told the password is
        // unencrypted on the LAN, in those words, and given the way out.
        let text = FleetWiFiRollout.cleartextDisclosure.lowercased()
        XCTAssertTrue(text.contains("unencrypted"))
        XCTAssertTrue(text.contains("password"))
        XCTAssertTrue(text.contains("bluetooth"))
        XCTAssertTrue(FleetWiFiRollout.cleartextDeclined.lowercased().contains("not sent"))
        XCTAssertTrue(FleetWiFiRollout.cleartextNote.lowercased().contains("unencrypted"))
    }

    func testPinnedTLSNeedsAnHTTPSBaseAndAParsablePin() {
        let fp = String(repeating: "ab", count: 32)
        XCTAssertTrue(FleetWiFiRollout.isPinnedTLS(url: URL(string: "https://192.168.1.9"), fingerprint: fp))
        XCTAssertFalse(FleetWiFiRollout.isPinnedTLS(url: URL(string: "http://192.168.1.9"), fingerprint: fp),
                       "a pin on a plain-http URL encrypts nothing")
        XCTAssertFalse(FleetWiFiRollout.isPinnedTLS(url: URL(string: "https://192.168.1.9"), fingerprint: nil))
        XCTAssertFalse(FleetWiFiRollout.isPinnedTLS(url: URL(string: "https://192.168.1.9"), fingerprint: "junk"))
        XCTAssertFalse(FleetWiFiRollout.isPinnedTLS(url: nil, fingerprint: fp))
    }

    func testAnUnknownSignalSortsBehindAKnownOne() {
        let plan = FleetWiFiRollout.plan([
            candidate("mystery"),
            candidate("known", rssi: -75),
        ])
        XCTAssertEqual(plan.pilot?.id, "known")
    }

    func testLanesAreNamedHonestly() {
        let plan = FleetWiFiRollout.plan([
            candidate("wap", pinned: true),
            candidate("display", updatable: false),
            candidate("basement", online: false),
        ])
        XCTAssertEqual(plan.handsOn.map(\.id), ["display"])
        XCTAssertEqual(plan.unreachable.map(\.id), ["basement"])
        XCTAssertEqual(plan.pushTargets.map(\.id), ["wap"])
    }

    func testAnAllHandsOnFleetHasNoPilot() {
        let plan = FleetWiFiRollout.plan([candidate("d1", updatable: false),
                                          candidate("d2", updatable: false)])
        XCTAssertNil(plan.pilot)
        XCTAssertTrue(plan.pushTargets.isEmpty)
    }

    // MARK: - the one safety rule

    func testTheFleetWaitsForProof() {
        XCTAssertFalse(FleetWiFiRollout.mayFanOut(pilotState: .sending))
        XCTAssertFalse(FleetWiFiRollout.mayFanOut(pilotState: .confirming))
        XCTAssertFalse(FleetWiFiRollout.mayFanOut(pilotState: .failed("no")))
        XCTAssertFalse(FleetWiFiRollout.mayFanOut(pilotState: nil))
        XCTAssertTrue(FleetWiFiRollout.mayFanOut(pilotState: .moved))
    }

    // MARK: - credential bounds (mirroring the firmware's)

    func testCredentialBoundsMatchTheFirmware() {
        XCTAssertNil(FleetWiFiRollout.credentialProblem(ssid: "Home", password: "hunter22"))
        XCTAssertNotNil(FleetWiFiRollout.credentialProblem(ssid: "", password: "x"))
        XCTAssertNotNil(FleetWiFiRollout.credentialProblem(ssid: "   ", password: "x"))
        XCTAssertNotNil(FleetWiFiRollout.credentialProblem(
            ssid: String(repeating: "s", count: 33), password: "x"))
        XCTAssertNil(FleetWiFiRollout.credentialProblem(
            ssid: String(repeating: "s", count: 32), password: "x"))
        XCTAssertNotNil(FleetWiFiRollout.credentialProblem(
            ssid: "Home", password: String(repeating: "p", count: 65)))
        XCTAssertNil(FleetWiFiRollout.credentialProblem(
            ssid: "Home", password: String(repeating: "p", count: 64)))
        // An open network's empty password is legal.
        XCTAssertNil(FleetWiFiRollout.credentialProblem(ssid: "Cafe", password: ""))
    }

    func testBoundsCountBytesNotGlyphs() {
        // The firmware counts bytes; four-byte emoji must not sneak past a
        // character count. 9 birds × 4 bytes = 36 > 32.
        let birds = String(repeating: "🐦", count: 9)
        XCTAssertNotNil(FleetWiFiRollout.credentialProblem(ssid: birds, password: "x"))
    }
}
