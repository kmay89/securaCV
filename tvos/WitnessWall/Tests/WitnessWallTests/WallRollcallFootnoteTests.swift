//  WallRollcallFootnoteTests.swift — a hub that lists only itself says which
//  setting.
//
//  The kernel's roll-call is its own row alone until the owner opts into the
//  full one (`api.fleet_rollcall`; the add-on's `fleet_rollcall`; the
//  sidecar's SECURACV_FLEET_ROLLCALL — tvos/discovery/DISCOVERY.md), and an
//  add-on with MQTT publishing off serves the same single row. A Wall that
//  shows one "Witness kernel" tile forever, with the fixing option named
//  nowhere on screen, is the failure these pin against.

import XCTest
@testable import WitnessWall

@MainActor
final class WallRollcallFootnoteTests: XCTestCase {
    private func fleet(_ json: String) throws -> FleetSnapshot {
        try JSONDecoder().decode(FleetSnapshot.self, from: Data(json.utf8))
    }

    func testAHubThatListsOnlyItselfIsSaidAndALoneCanaryIsNot() throws {
        let kernelAlone = try fleet(#"{"kernel":"witness-kernel","devices":[{"name":"Witness kernel","online":true,"product":"witness-kernel"}]}"#)
        XCTAssertTrue(WallView.hubListsOnlyItself(kernelAlone),
                      "one row, and it is the kernel itself: the roll-call is off, or MQTT publishing is")

        let loneCanary = try fleet(#"{"kernel":"canary-wap","devices":[{"name":"Front Door","online":true,"product":"canary-wap"}]}"#)
        XCTAssertFalse(WallView.hubListsOnlyItself(loneCanary),
                       "one Canary fronting its own fleet is hardware — its roll-call is complete")
        let untyped = try fleet(#"{"kernel":"kitchen-hub","devices":[{"name":"Front Door"}]}"#)
        XCTAssertFalse(WallView.hubListsOnlyItself(untyped), "a row that names no product is not the kernel's")

        let full = try fleet(#"{"kernel":"witness-kernel","devices":[{"name":"Witness kernel","online":true,"product":"witness-kernel"},{"name":"Front Door","online":true,"product":"canary-wap"}]}"#)
        XCTAssertFalse(WallView.hubListsOnlyItself(full), "a second row is the roll-call working")
        let none = try fleet(#"{"kernel":"witness-kernel","devices":[]}"#)
        XCTAssertFalse(WallView.hubListsOnlyItself(none), "no rows is the empty wall, which has its own words")
    }

    func testTheFootnoteNamesTheSwitchAndTheOtherCause() {
        let note = WallView.hubListsOnlyItselfFootnote
        XCTAssertTrue(note.contains("fleet_rollcall: full"), "the add-on option, by name, is the point of the sentence")
        XCTAssertTrue(note.contains("MQTT"), "an add-on with publishing off serves the same single row")
        XCTAssertTrue(note.hasPrefix("Your hub lists only itself."), "it says what is on screen before what to do")
    }
}
