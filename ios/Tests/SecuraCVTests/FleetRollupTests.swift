// FleetRollupTests.swift
//
// The glance math behind every ambient surface — the Dynamic Island state and
// the wrist snapshot are BOTH built from FleetRollup, so these tests are the
// contract that the phone's island and the watch can never tell different
// stories. Each test names the rule it guards.

import XCTest
@testable import SecuraCV

final class FleetRollupTests: XCTestCase {
    private func witness(_ id: String,
                         severity: Severity = .ok,
                         link: Liveness = .online,
                         badge: TrustBadge = .verified,
                         tamper: Bool = false,
                         mutedUntil: Date? = nil) -> Witness {
        var w = Witness(id: id)
        w.name = id
        w.lastEventSeverity = severity
        w.link = link
        w.badge = badge
        w.tamper = tamper
        w.mutedUntil = mutedUntil
        return w
    }

    func testAllQuietWhenNothingIsWrong() {
        let fleet = [witness("porch"), witness("garage")]
        XCTAssertEqual(FleetRollup.worst(fleet), .ok)
        XCTAssertEqual(FleetRollup.headline(fleet), "All quiet")
        XCTAssertEqual(FleetRollup.healthyCount(fleet), 2)
    }

    func testAnEmptyFleetIsQuietNotBroken() {
        XCTAssertEqual(FleetRollup.worst([]), .ok)
        XCTAssertEqual(FleetRollup.headline([]), "All quiet")
        XCTAssertEqual(FleetRollup.healthyCount([]), 0)
    }

    func testHealthyExcludesDarkTamperedAndFailedSignature() {
        let fleet = [
            witness("fine"),
            witness("dark", link: .lost),
            witness("tampered", tamper: true),
            witness("forged", badge: .failed),
        ]
        XCTAssertEqual(FleetRollup.healthyCount(fleet), 1)
    }

    func testMuteCapsNaggingButNeverChangesHealth() {
        // A muted witness with a warn-level event: quieter in the roll-up,
        // identical in the health count.
        let muted = witness("muted", severity: .warn,
                            mutedUntil: Date().addingTimeInterval(3_600))
        XCTAssertEqual(FleetRollup.healthyCount([muted]), 1)
        XCTAssertEqual(FleetRollup.worst([muted]), .notice)   // capped, not hidden
    }

    func testTheHeadlineNamesTheHottestWitness() {
        let fleet = [witness("porch"), witness("garage", severity: .alert)]
        XCTAssertEqual(FleetRollup.headline(fleet), "garage • Alert")
    }

    func testTamperPunchesThroughMuteEverywhere() {
        let muted = witness("shed", tamper: true,
                            mutedUntil: Date().addingTimeInterval(3_600))
        XCTAssertEqual(FleetRollup.worst([muted]), .tamper)
        XCTAssertEqual(FleetRollup.healthyCount([muted]), 0)
    }

    func testTheIslandStateAgreesWithTheRollup() {
        let fleet = [
            witness("porch", severity: .notice),
            witness("dark", link: .offline),
        ]
        let state = FleetActivityAttributes.State(fleet: fleet, lastVerifiedAgo: 42)
        XCTAssertEqual(state.severityRaw, FleetRollup.worst(fleet).rawValue)
        XCTAssertEqual(state.healthy, FleetRollup.healthyCount(fleet))
        XCTAssertEqual(state.total, fleet.count)
        XCTAssertEqual(state.headline, FleetRollup.headline(fleet))
        XCTAssertEqual(state.lastVerifiedAgo, 42)
    }
}
