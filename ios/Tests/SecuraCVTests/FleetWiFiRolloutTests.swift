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
                           rssi: Int? = nil) -> FleetWiFiRollout.Candidate {
        FleetWiFiRollout.Candidate(id: id, name: id, updatable: updatable,
                                   online: online, bleReachable: ble, rssiDBM: rssi)
    }

    // MARK: - lanes

    func testAnOnlineWAPRidesHTTP() {
        XCTAssertEqual(candidate("a").path, .http)
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

    func testAnUnknownSignalSortsBehindAKnownOne() {
        let plan = FleetWiFiRollout.plan([
            candidate("mystery"),
            candidate("known", rssi: -75),
        ])
        XCTAssertEqual(plan.pilot?.id, "known")
    }

    func testLanesAreNamedHonestly() {
        let plan = FleetWiFiRollout.plan([
            candidate("wap"),
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
