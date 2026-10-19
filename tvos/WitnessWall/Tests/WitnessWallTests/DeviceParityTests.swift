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

    // MARK: - the birth day

    /// Verbatim `fleet_selfreport_build()` output for a Canary that met a clock
    /// the evening it was set up — the same literal as the iPhone's
    /// FleetSelfReportTests.bornBody and the first birth-day contract vector.
    private let bornJSON = #"""
    {"kernel":"Front Door","verified_through":"now","devices":[{"name":"Front Door","online":true,"chain":"ok","product":"canary-wap","chain_height":42,"born_day":20673,"born_exact":true}]}
    """#

    /// And one that sat on a shelf first (FleetSelfReportTests.firstDatedBody).
    private let firstDatedJSON = #"""
    {"kernel":"Shed","verified_through":"now","devices":[{"name":"Shed","online":true,"chain":"ok","product":"canary-wap","chain_height":7,"born_day":20666,"born_exact":false}]}
    """#

    private static let en = Locale(identifier: "en_US")

    /// A born day is a UTC day, so the Wall formats it in UTC. CI's macOS
    /// runners run in UTC, where a formatter that slipped back to the local
    /// zone would print the same day and pass, so each birth-day test that
    /// reads the words first moves this process west of Greenwich, where the
    /// local day of a UTC midnight is the day before, and puts the zone back
    /// when the test ends.
    private func pinAZoneWestOfUTC() throws {
        let saved = NSTimeZone.default
        NSTimeZone.default = try XCTUnwrap(TimeZone(identifier: "America/Los_Angeles"))
        addTeardownBlock { NSTimeZone.default = saved }
    }

    /// The en_US abbreviated form of a vector's `utc_date` ("2026-08-08"
    /// reads "Aug 8, 2026"), spelled out by hand so the expected words never
    /// come from the formatter under test.
    private func enUSDay(_ iso: String) -> String? {
        let parts = iso.split(separator: "-").compactMap { Int($0) }
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        guard parts.count == 3, (1...12).contains(parts[1]) else { return nil }
        return "\(months[parts[1] - 1]) \(parts[2]), \(parts[0])"
    }

    /// The UTC calendar day of a born day, read independently of the Wall's
    /// formatter: a born day is a UTC day, so a TV west of Greenwich must not
    /// show the day before.
    private func utcDay(_ date: Date) -> String {
        var utc = Calendar(identifier: .gregorian)
        utc.timeZone = TimeZone(secondsFromGMT: 0)!
        let c = utc.dateComponents([.year, .month, .day], from: date)
        return String(format: "%04d-%02d-%02d", c.year ?? 0, c.month ?? 0, c.day ?? 0)
    }

    /// "Born" is the device's word, and only when the device says the day is
    /// exact.
    func testAnExactBirthDayReadsBorn() throws {
        try pinAZoneWestOfUTC()
        let d = try device(from: bornJSON)
        XCTAssertEqual(d.bornDay, 20673)
        XCTAssertEqual(d.bornExact, true)
        XCTAssertEqual(d.bornLabel, "Born")
        XCTAssertEqual(utcDay(try XCTUnwrap(d.bornOn)), "2026-08-08")
        XCTAssertEqual(d.wallBornLine(locale: Self.en), "Born Aug 8, 2026")
        XCTAssertNil(d.wallBornNote, "an exact day needs no explanation")
    }

    /// A day learned late is shown — it bounds the device's age — but never
    /// called a birthday.
    func testAFirstDatedDayIsNeverCalledABirthday() throws {
        try pinAZoneWestOfUTC()
        let d = try device(from: firstDatedJSON)
        XCTAssertEqual(d.bornDay, 20666)
        XCTAssertEqual(d.bornExact, false)
        XCTAssertEqual(d.bornLabel, "First dated")
        XCTAssertEqual(d.wallBornLine(locale: Self.en), "First dated Aug 1, 2026")
        XCTAssertNotNil(d.wallBornNote, "the label says why it is not 'Born'")
    }

    /// A device that has never met a clock (or a display, which has no key of
    /// its own) sends neither key, and the Wall draws no line at all — never
    /// the epoch.
    func testNoBirthDayDrawsNothing() throws {
        let d = try device(from: displayJSON)
        XCTAssertNil(d.bornDay)
        XCTAssertNil(d.bornExact)
        XCTAssertNil(d.bornLabel)
        XCTAssertNil(d.wallBornLine())
        XCTAssertNil(d.wallBornNote)
    }

    /// The birth day is written once on the device and never restated, so a
    /// source that went dark keeps it with the other durable facts.
    func testABirthDaySurvivesTheSourceGoingDark() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(bornJSON.utf8))
        let remembered = snapshot.withEveryDeviceOffline().devices[0]
        XCTAssertFalse(remembered.online)
        XCTAssertEqual(remembered.bornDay, 20673)
        XCTAssertEqual(remembered.bornExact, true)
    }

    /// witness-core's fleet contract vectors, which carry a `born` line per
    /// row (the label and the UTC day, or null). The Wall decodes the core's
    /// `normalized` snapshot in production, so that is replayed first; the
    /// raw `input` goes through this decoder's own backstop and must reach
    /// the same birth day — the window edges and the malformed rows included.
    func testEveryContractVectorsBornLineReadsTheSameHere() throws {
        try pinAZoneWestOfUTC()
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // WitnessWallTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // WitnessWall
            .deletingLastPathComponent()   // tvos
            .deletingLastPathComponent()   // repo root
            .appendingPathComponent("tvos/witness-core/tests/fixtures/fleet_contract_vectors.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "repo checkout not visible from the test host; " +
                          "witness-core's fleet_contract test remains the gate for these vectors")
        let file = try XCTUnwrap(try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
        let vectors = try XCTUnwrap(file["vectors"] as? [[String: Any]])
        var lines = 0
        for vector in vectors {
            let name = vector["name"] as? String ?? "?"
            let normalized = try XCTUnwrap(vector["normalized"])
            let snapshot = try JSONDecoder().decode(
                FleetSnapshot.self, from: JSONSerialization.data(withJSONObject: normalized))
            let born = vector["born"] as? [Any] ?? Array(repeating: NSNull(), count: snapshot.devices.count)
            XCTAssertEqual(born.count, snapshot.devices.count, "\(name): one born entry per row")
            for (i, d) in snapshot.devices.enumerated() where i < born.count {
                if let want = born[i] as? [String: String] {
                    XCTAssertEqual(d.bornLabel, want["label"], "\(name) row \(i): label")
                    XCTAssertEqual(d.bornOn.map { self.utcDay($0) }, want["utc_date"], "\(name) row \(i): UTC day")
                    // The words the detail screen shows, in a zone west of UTC.
                    let words = try XCTUnwrap(want["utc_date"].flatMap { self.enUSDay($0) },
                                              "\(name) row \(i): utc_date is not YYYY-MM-DD")
                    XCTAssertEqual(d.wallBornLine(locale: Self.en), "\(want["label"] ?? "?") \(words)",
                                   "\(name) row \(i): the detail screen's line")
                    // The reason under "First dated" is the device's own: only
                    // a row that said born_exact false gets it, never one that
                    // sent its day without the flag.
                    XCTAssertEqual(d.wallBornNote != nil, d.bornExact == false,
                                   "\(name) row \(i): the note follows the device's own flag")
                    lines += 1
                } else {
                    XCTAssertNil(d.wallBornLine(), "\(name) row \(i): no day, so no line")
                }
            }
            // The backstop: the raw bytes, decoded here without the core.
            guard let input = (vector["input"] as? String)?.data(using: .utf8),
                  (try? JSONSerialization.jsonObject(with: input)) is [String: Any] else { continue }
            let raw = try JSONDecoder().decode(FleetSnapshot.self, from: input)
            XCTAssertEqual(raw.devices.map(\.bornDay), snapshot.devices.map(\.bornDay),
                           "\(name): this decoder and the core disagree about a birth day")
            XCTAssertEqual(raw.devices.map(\.bornExact), snapshot.devices.map(\.bornExact),
                           "\(name): this decoder and the core disagree about born_exact")
        }
        XCTAssertGreaterThanOrEqual(lines, 4, "the vectors should pin Born and First dated lines")
    }

    // MARK: - the fleet-level verdict

    /// The count under the fleet name must not treat a chainless display as a
    /// device needing attention.
    func testAFleetOfDisplaysNeedsNoAttention() throws {
        let snapshot = try JSONDecoder().decode(FleetSnapshot.self, from: Data(displayJSON.utf8))
        XCTAssertFalse(snapshot.hasChainTrouble)
        XCTAssertEqual(snapshot.onlineCount, 1)
    }

    // MARK: - the hub's own row

    /// A Wall pointed at a hub shows the kernel's own row first (src/api
    /// mod.rs: name "Witness kernel", product KERNEL_NAME). It is software:
    /// it gets a name, and it is never told it is waiting for a hardware
    /// figure the way an unknown DEVICE honestly is.
    func testTheKernelsOwnRowIsNamedAndKnownToBeSoftware() throws {
        let d = try device(from: #"{"kernel":"witness-kernel","verified_through":"now","devices":[{"name":"Witness kernel","online":true,"chain":"ok","product":"witness-kernel"}]}"#)
        XCTAssertEqual(d.productName, "SecuraCV witness kernel")
        XCTAssertTrue(DeviceNaming.isSoftware(published: d.product))
        XCTAssertNil(d.figure, "no hardware to draw — the server symbol stands in")
    }
}
