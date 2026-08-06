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
            AlertCenter.unknowableFromAway(presence: .away, isDark: true, tamper: false),
            "away + dark + no tamper is exactly the unknowable case")
        XCTAssertFalse(
            AlertCenter.unknowableFromAway(presence: .home, isDark: true, tamper: false),
            "at home, darkness is a real observation and must be reported")
    }

    func testTamperSurvivesTheDistance() {
        // The device said so itself before going quiet — distance does not
        // make that claim less true, and it is the one that matters most.
        XCTAssertFalse(
            AlertCenter.unknowableFromAway(presence: .away, isDark: true, tamper: true),
            "a tampered Canary reports from anywhere, dark or not")
    }

    func testALiveWitnessIsNeverSuppressed() {
        for presence: HomePresence in [.home, .away, .unknown] {
            XCTAssertFalse(
                AlertCenter.unknowableFromAway(presence: presence, isDark: false, tamper: false),
                "only darkness is unknowable from away; everything else speaks")
        }
    }

    // MARK: - the total-outage hole

    func testWholeHouseBlackoutIsNeverMistakenForBeingAway() {
        // THE bug, and it was worse than it looked. `awayFromHome` was derived
        // from `!seesFleet`, and darkness is the ONLY thing that makes
        // `seesFleet` false — so the away guard could fire in exactly one
        // situation: no Canary answering at all. That is not the drive to
        // work. That is the one-Canary household whose Canary just died, and
        // the power cut that took every device at once — the event the owner
        // named FIRST when asking for any of this.
        XCTAssertFalse(
            AlertCenter.unknowableFromAway(presence: .unknown, isDark: true, tamper: false),
            "a phone on the fleet's own network must report the blackout")
    }

    func testUnknownIsNeverTreatedAsAway() {
        // The three-state split only pays off if `.unknown` refuses to
        // suppress. If it ever gains that power the original bug is back
        // under a new name.
        XCTAssertFalse(HomePresence.unknown.maySuppressDarkness)
        XCTAssertFalse(HomePresence.home.maySuppressDarkness)
        XCTAssertTrue(HomePresence.away.maySuppressDarkness)
    }

    func testPresenceNeedsPositiveEvidenceToSayAway() {
        // Seeing a Canary is the best evidence there is, and it wins outright.
        XCTAssertEqual(
            HomePresence.evaluate(seesFleet: true, onWiFi: false, sawFleetOnThisNetwork: false),
            .home, "a Canary answering outranks every other signal")

        // Cellular: certainly not on the home LAN. This is the storm case.
        XCTAssertEqual(
            HomePresence.evaluate(seesFleet: false, onWiFi: false, sawFleetOnThisNetwork: true),
            .away)

        // On the network the fleet lives on, and nothing answers. Blackout
        // until proven otherwise — and "otherwise" is not ours to assume.
        XCTAssertEqual(
            HomePresence.evaluate(seesFleet: false, onWiFi: true, sawFleetOnThisNetwork: true),
            .unknown)

        // On a Wi-Fi the fleet has never once answered on: someone else's
        // house. No SSID read, no location — just "did they ever answer here".
        XCTAssertEqual(
            HomePresence.evaluate(seesFleet: false, onWiFi: true, sawFleetOnThisNetwork: false),
            .away)
    }

    func testPresenceIsTotal() {
        for seen in [true, false] {
            for wifi in [true, false] {
                for sawHere in [true, false] {
                    let p = HomePresence.evaluate(seesFleet: seen, onWiFi: wifi,
                                                  sawFleetOnThisNetwork: sawHere)
                    if seen { XCTAssertEqual(p, .home) }
                    // The invariant that actually protects the user: never
                    // suppress while a Canary is answering.
                    if seen { XCTAssertFalse(p.maySuppressDarkness) }
                }
            }
        }
    }

    @MainActor
    func testVantageForgetsTheFleetOnLeavingANetwork() {
        // The latch is a claim about the CURRENT attachment. Carrying it
        // across a network change would let a guest network inherit home's
        // "the fleet answers here", turning every away trip into .unknown.
        let vantage = NetworkVantage.shared
        vantage._setForTesting(onWiFi: true, sawFleet: true)
        XCTAssertTrue(vantage.sawFleetOnThisNetwork)
        vantage._simulatePathChangeForTesting(onWiFi: true)
        XCTAssertFalse(vantage.sawFleetOnThisNetwork,
                       "a new attachment starts with no claim about the fleet")
    }

    func testOnWiFiIsNotClaimedFromAcrossTown() {
        // The delivery label is a claim about WHERE the phone was, and it was
        // only ever true because `awayFromHome` was hardcoded false. An away
        // phone still posts locally for what it can genuinely observe, and
        // stamping that "On Wi-Fi" would be a false statement on the one tab
        // whose job is answering "did this actually reach me?" honestly.
        XCTAssertEqual(AlertDelivery.onLAN.label, "On Wi-Fi")
        XCTAssertEqual(AlertDelivery.away.label, "Away")
        XCTAssertTrue(AlertDelivery.away.rawValue > AlertDelivery.onLAN.rawValue,
                      "the ledger only moves delivery up, so away must outrank on-LAN")
    }
}
