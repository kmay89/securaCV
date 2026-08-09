//  DeviceParityTests.swift — the Wall describes a device the way the phone does.
//
//  The Witness Wall is opened for a few hours every several weeks, which is
//  exactly long enough for it to quietly re-decide things the iPhone had
//  already decided. It had done all three:
//
//    * printed the raw wire string, so a television read "canary-nightstand7"
//      where the phone read "Canary Nightstand 7";
//    * drawn a colored dot where the phone drew the device;
//    * treated `chain: "unknown"` as a verification FAILURE, painting every
//      display in the fleet orange with "Record didn't verify" for a chain
//      those devices never claimed to have.
//
//  The structural fix is that the deciding code is now COMPILED here rather
//  than reimplemented (project.yml's parity block, guarded by
//  scripts/lint_apple_parity.py). These tests are the behavioral half: they
//  run against the SAME bytes a display actually sends, so if the two surfaces
//  ever diverge again this is what says so.

import XCTest
@testable import WitnessWall

final class DeviceParityTests: XCTestCase {

    /// Verbatim stdout of `fleet_selfreport_build()` from
    /// firmware/common/fleet_selfreport — the same literal the iPhone's
    /// FleetSelfReportTests pins, which is the point: one set of bytes, two
    /// surfaces, one answer.
    private let displayJSON = #"""
    {"kernel":"canary_nightstand7_001","verified_through":"now","devices":[{"name":"canary_nightstand7_001","online":true,"chain":"unknown","product":"canary-nightstand7","hw":"waveshare-esp32s3-lcd7","hub":"none"}]}
    """#

    private func device(from json: String) throws -> FleetSnapshot.Device {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(json.utf8))
        return try XCTUnwrap(snapshot.devices.first)
    }

    // MARK: - the honesty bug this suite was born from

    /// A display holds no witness chain of its own, says "unknown", and must
    /// NOT be drawn as a verification failure.
    func testAnUnknownChainIsNotTrouble() throws {
        let d = try device(from: displayJSON)
        XCTAssertEqual(d.chain, "unknown")
        XCTAssertFalse(d.chainIsTroubled,
                       "\"unknown\" is an absent claim, not a failure — reading it as "
                       + "one painted every display in the fleet orange")
    }

    /// A missing field is the same absent claim.
    func testAMissingChainIsNotTrouble() throws {
        let d = try device(from: #"{"devices":[{"name":"Porch"}]}"#)
        XCTAssertFalse(d.chainIsTroubled)
    }

    /// And a real failure still is one — the fix must not have blunted it.
    func testARealChainFailureIsStillTrouble() throws {
        let d = try device(from: #"{"devices":[{"name":"A","chain":"broken"}]}"#)
        XCTAssertTrue(d.chainIsTroubled)
    }

    // MARK: - naming

    /// The product NAME, never the identifier. This is the one most visible
    /// from a couch.
    func testTheProductNameIsAName() throws {
        let d = try device(from: displayJSON)
        XCTAssertEqual(d.productName, "Canary Nightstand 7")
        XCTAssertNotEqual(d.productName, d.product,
                          "the wire string is an identifier and must not reach the screen")
    }

    /// A type this build has never heard of yields nil, so the caller says
    /// something coarse rather than printing the raw string.
    func testAnUnknownProductNamesNothingRatherThanGuessing() throws {
        let d = try device(from: #"{"devices":[{"name":"X","product":"canary-something-new"}]}"#)
        XCTAssertNil(d.productName)
    }

    // MARK: - the picture

    /// The board resolves the figure, which is the whole reason `hw` is
    /// carried through the Rust normalizer.
    func testTheBoardResolvesAFigure() throws {
        let d = try device(from: displayJSON)
        XCTAssertEqual(d.hw, "waveshare-esp32s3-lcd7")
        XCTAssertEqual(d.figure?.id, "device.canary-display-dash7")
    }

    /// The display line decodes as a display here exactly as it does on the
    /// phone — a strict rawValue lookup drops all of it to `.unknown`.
    func testTheDisplayLineDecodesTheSameWayItDoesOnThePhone() throws {
        let d = try device(from: displayJSON)
        XCTAssertEqual(d.deviceType, .display)
    }

    /// Old firmware carries neither field, and that must stay a clean
    /// fallback rather than a crash or an invented picture.
    func testADeviceOnOlderFirmwareDegradesHonestly() throws {
        let d = try device(from: #"{"devices":[{"name":"Front Door","product":"canary-wap"}]}"#)
        XCTAssertNil(d.hw)
        XCTAssertNil(d.hub)
        XCTAssertEqual(d.productName, "Canary WAP")
        XCTAssertEqual(d.figure?.id, "device.canary-wap", "the coarse type still draws")
        XCTAssertEqual(d.hubState, HubState.unknown)
        XCTAssertFalse(d.hubState.needsAttention, "silence is not a problem to report")
    }

    /// The phone reports a device that omits a field rather than dropping it.
    /// The Wall used strict synthesis, where one silent Canary throws — and a
    /// throw here loses the whole snapshot, blanking the television for the
    /// devices that DID answer.
    func testASilentFieldCostsTheDeviceNothing() throws {
        let snapshot = try JSONDecoder().decode(
            FleetSnapshot.self,
            from: Data(#"{"devices":[{"name":"Attic"},{"name":"Porch","online":true}]}"#.utf8))
        XCTAssertEqual(snapshot.devices.map(\.name), ["Attic", "Porch"],
                       "a device that omits a field is reported, not dropped")
        XCTAssertFalse(snapshot.devices[0].online,
                       "absent is not a presence claim — never rendered as online")
        XCTAssertEqual(snapshot.onlineCount, 1)
    }

    /// An empty string is the same absent answer as a missing key, folded once
    /// here so no caller has to prove that "" is not a board id.
    func testAnEmptyBoardIsAbsentRatherThanABoardNamedNothing() throws {
        let d = try device(from: #"{"devices":[{"name":"A","hw":"","hub":""}]}"#)
        XCTAssertNil(d.hw)
        XCTAssertNil(d.hub)
        XCTAssertEqual(d.hubState, HubState.unknown)
    }

    // MARK: - the hub

    func testHubStateIsReadTheSameWayThePhoneReadsIt() throws {
        let d = try device(from: displayJSON)
        XCTAssertEqual(d.hubState, HubState.absent)
        XCTAssertTrue(d.hubState.needsAttention)

        let connected = try device(from: #"{"devices":[{"name":"A","hub":"ok"}]}"#)
        XCTAssertEqual(connected.hubState, HubState.ok)
        XCTAssertFalse(connected.hubState.needsAttention)
    }

    // MARK: - the fleet-level verdict

    /// The count under the fleet name must not treat a chainless display as a
    /// device needing attention.
    func testAFleetOfDisplaysNeedsNoAttention() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(displayJSON.utf8))
        XCTAssertFalse(snapshot.hasChainTrouble)
        XCTAssertEqual(snapshot.onlineCount, 1)
    }
}
