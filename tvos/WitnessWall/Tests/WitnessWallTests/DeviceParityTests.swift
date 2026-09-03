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

    // MARK: - the wellbeing words (the aggregated body)

    /// A display aggregating its fleet: self row (no wellbeing keys), a sense
    /// peer carrying the coarse room words, and a (future) camera-line row
    /// carrying a seeing claim. Verbatim stdout of the firmware's
    /// open/append/close composition — the SAME literal the iPhone's
    /// FleetSelfReportTests.aggregatedBody pins and the Rust contract vectors
    /// replay (tvos/witness-core/tests/fixtures/fleet_contract_vectors.json):
    /// one set of bytes, three readers, one answer.
    private let aggregatedJSON = #"""
    {"kernel":"Hallway Glass","verified_through":"now","devices":[{"name":"Hallway Glass","online":true,"chain":"unknown","product":"canary-dash7","hw":"waveshare-esp32s3-lcd7","hub":"ok"},{"name":"Bedroom","online":true,"chain":"unknown","product":"canary-sense","presence":"present","occupants":"1","breathing":true},{"name":"Driveway","online":true,"chain":"ok","product":"canary-vision","chain_height":512,"seeing":"package","seeing_score":87}]}
    """#

    /// The words fold to the same verdicts the phone reaches, and the rows
    /// that said nothing stay silent — absence is "cannot say", never an
    /// empty calm room.
    func testWellbeingWordsDecodeFromTheDisplaysAggregatedBody() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(aggregatedJSON.utf8))
        XCTAssertEqual(snapshot.devices.count, 3)

        // The self row carries no wellbeing keys — and none may be invented.
        let glass = snapshot.devices[0]
        XCTAssertNil(glass.radarPresent)
        XCTAssertNil(glass.radarOccupants)
        XCTAssertNil(glass.breathing)
        XCTAssertNil(glass.seeing)
        XCTAssertNil(glass.wallWellbeingLine,
                     "a row without the keys draws NOTHING — never an empty calm room")

        // The sense peer's words fold exactly as the phone folds them.
        let bedroom = snapshot.devices[1]
        XCTAssertEqual(bedroom.presence, "present")
        XCTAssertEqual(bedroom.radarPresent, true)
        XCTAssertEqual(bedroom.radarOccupants, 1)
        XCTAssertEqual(bedroom.breathing, true)

        // The seeing claim survives with its score.
        let driveway = snapshot.devices[2]
        XCTAssertEqual(driveway.seeing, "package")
        XCTAssertEqual(driveway.seeingScore, 87)
    }

    /// The one quiet line the card and the detail screen share, pinned so the
    /// phrasing can't drift per-surface.
    func testTheWellbeingLineSaysTheRoomCoarsely() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(aggregatedJSON.utf8))
        XCTAssertEqual(snapshot.devices[1].wallWellbeingLine,
                       "Someone present · 1 in the room · breathing rhythm sensed")
        XCTAssertEqual(snapshot.devices[2].wallWellbeingLine, "Seeing a package · 87%")
    }

    /// Words this build has never heard fold to nil verdicts and to no line
    /// at all, never to a guess — the chain/hub tolerance rule, held for the
    /// new keys. And a score outside 1…100 reads as unscored (the phone's
    /// "150 is not a percentage" rule).
    func testUnknownWellbeingWordsFoldToNothingNotAGuess() throws {
        let d = try device(from: #"{"devices":[{"name":"K","online":true,"presence":"levitating","occupants":"many","seeing":"face","seeing_score":150}]}"#)
        XCTAssertNil(d.radarPresent, "an unknown presence word must not read as either answer")
        XCTAssertNil(d.radarOccupants)
        XCTAssertNil(d.seeingScore, "150 is not a percentage")
        XCTAssertNil(d.wallWellbeingLine,
                     "a class outside the vocabulary renders as nothing (Invariant II)")
    }

    /// A source that stopped answering keeps its devices on the wall, but its
    /// remembered wellbeing claims do not survive: "someone present" is the
    /// most present-tense fact on the wire, and a stale claim omits rather
    /// than lies. Durable facts (board, hub standing, chain word) stay.
    func testARememberedWellbeingClaimDoesNotSurviveTheSourceGoingDark() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(aggregatedJSON.utf8))
        let remembered = snapshot.withEveryDeviceOffline()
        let bedroom = remembered.devices[1]
        XCTAssertFalse(bedroom.online)
        XCTAssertNil(bedroom.presence)
        XCTAssertNil(bedroom.occupants)
        XCTAssertNil(bedroom.breathing)
        XCTAssertNil(remembered.devices[2].seeing)
        XCTAssertNil(remembered.devices[2].seeingScore)
        XCTAssertEqual(remembered.devices[0].hw, "waveshare-esp32s3-lcd7",
                       "the durable facts still draw the device")
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
