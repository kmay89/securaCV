//  WallTimelineTests.swift — the Wall's timeline: when it may be drawn, and
//  every decision the remote-driven view makes, as data.
//
//  The load-bearing rule: a timeline is drawn ONLY from a sealed log this TV
//  walked and verified against the key it pinned at pairing. An unpinned
//  walk, a failed walk, a changed key, a lost source and a body that is not
//  a sealed log all leave it empty — the ribbon never wears an integrity
//  claim the Wall has not earned.

import XCTest
@testable import WitnessWall

@MainActor
final class WallTimelineTests: XCTestCase {
    private let fleet = #"{"kernel":"kitchen-hub","devices":[{"name":"Front Door","online":true}]}"#
    private let token = String(repeating: "7", count: 64)

    /// A single-source Wall over a transport serving `sealedLog`, paired with
    /// `pinnedKey` when one is given (pairings in memory).
    private func wall(serving sealedLog: String?, pinnedKey: String?,
                      fleetAnswers: [Result<String, Error>]? = nil) -> (WallModel, SealedLogTransport) {
        let defaults = UserDefaults(suiteName: UUID().uuidString)!
        defaults.set(["canary.local:8799"], forKey: "SecuraCVWallSources")
        let transport = SealedLogTransport(fleet: fleetAnswers ?? [.success(fleet)], sealedLog: sealedLog)
        let model = WallModel(transport: transport, defaults: defaults, pollInterval: 0.01,
                              pairings: PairedSourceStore(secrets: MemoryPairingSecrets()),
                              discover: { _ in [] })
        if let pinnedKey {
            XCTAssertNil(model.pair(receiptText: receiptJSON(token: token, key: pinnedKey)))
        }
        return (model, transport)
    }

    // MARK: - when the model may hold a timeline

    func testAVerifiedLogUnderThePinnedKeyBecomesTheTimeline() async throws {
        let vector = try sharedSealedLogVector()
        let key = try XCTUnwrap(VerificationStanding.servedKey(in: vector))
        let (m, _) = wall(serving: vector, pinnedKey: key)
        await m.refreshOnce()

        XCTAssertEqual(m.standing, .verified)
        XCTAssertEqual(m.timeline.map(\.kind), [.event, .event, .heartbeat])
        XCTAssertEqual(m.timeline.map(\.label),
                       ["Large object crossed boundary", "Large object crossed boundary", "Heartbeat"])
        XCTAssertEqual(m.timeline.map(\.zone), ["zone:a", "zone:b", ""])
        XCTAssertEqual(m.timeline.map(\.t0), [1_700_000_400, 1_700_001_000, 1_700_001_600],
                       "the buckets the kernel sealed, never a finer time")
        XCTAssertEqual(m.timeline.map(\.size), [600, 600, 600])
        XCTAssertEqual(m.timelineUnparsed, 0)
    }

    func testAnUnpinnedWalkDrawsNoTimelineEvenWhenItPasses() async throws {
        let vector = try sharedSealedLogVector()
        let (m, _) = wall(serving: vector, pinnedKey: nil)
        await m.refreshOnce()

        XCTAssertEqual(m.report?.ok, true)
        XCTAssertEqual(m.standing, .unpaired)
        XCTAssertTrue(m.timeline.isEmpty, "a ribbon would claim what the key was never pinned to say")
    }

    func testAChangedKeyDrawsNoTimeline() async throws {
        let vector = try sharedSealedLogVector()
        let (m, _) = wall(serving: vector, pinnedKey: String(repeating: "1", count: 64))
        await m.refreshOnce()

        XCTAssertTrue(m.standing.isAlarm)
        XCTAssertTrue(m.timeline.isEmpty)
    }

    func testATamperedLogDrawsNoTimeline() async {
        let key = "5866666666666666666666666666666666666666666666666666666666666666"
        let tampered = """
        {"verifying_key":"\(key)","entries":[{"id":7,
          "payload":"{\\"record_type\\":\\"heartbeat\\",\\"time_bucket\\":{\\"start_epoch_s\\":1700000400,\\"size_s\\":600}}",
          "prev_hash":"\(String(repeating: "a", count: 64))",
          "entry_hash":"\(String(repeating: "b", count: 64))",
          "signature":"\(String(repeating: "c", count: 128))"}]}
        """
        let (m, _) = wall(serving: tampered, pinnedKey: key)
        await m.refreshOnce()

        XCTAssertEqual(m.report?.ok, false)
        XCTAssertEqual(m.standing, .failedAgainstPin)
        XCTAssertTrue(m.timeline.isEmpty, "a failed walk shows the alarm banner, never a ribbon")
    }

    func testABodyThatIsNotASealedLogDrawsNoTimeline() async {
        let (m, _) = wall(serving: "<html>Sign in</html>", pinnedKey: String(repeating: "1", count: 64))
        await m.refreshOnce()

        XCTAssertNil(m.report)
        XCTAssertTrue(m.timeline.isEmpty)
    }

    func testLosingTheSourceClearsTheTimeline() async throws {
        let vector = try sharedSealedLogVector()
        let key = try XCTUnwrap(VerificationStanding.servedKey(in: vector))
        let (m, _) = wall(serving: vector, pinnedKey: key,
                          fleetAnswers: [.success(fleet), .failure(FleetError.unreachable("gone"))])
        await m.refreshOnce()
        XCTAssertFalse(m.timeline.isEmpty)

        await m.refreshOnce()
        guard case .stale = m.state else { return XCTFail("expected .stale, got \(m.state)") }
        XCTAssertTrue(m.timeline.isEmpty, "a remembered timeline is not a current one")
    }

    func testConnectingElsewhereClearsTheTimeline() async throws {
        let vector = try sharedSealedLogVector()
        let key = try XCTUnwrap(VerificationStanding.servedKey(in: vector))
        let (m, _) = wall(serving: vector, pinnedKey: key)
        await m.refreshOnce()
        XCTAssertFalse(m.timeline.isEmpty)

        m.connect(to: "192.168.1.30:8799")
        m.stop()
        XCTAssertTrue(m.timeline.isEmpty, "one source's record never draws under another's fleet")
    }

    // MARK: - the view's decisions, as data

    private let utc = TimeZone(secondsFromGMT: 0)!
    private var utcCalendar: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = utc
        return calendar
    }

    /// Two days in UTC: day one has buckets 6 and 40 (a gap in 40), day two
    /// has bucket 10 (tamper) — plus a heartbeat that lights nothing.
    private var twoDays: [TimelineRecord] {
        let day1 = 1_750_809_600   // 2025-06-25T00:00:00Z
        let day2 = day1 + 86_400
        return [
            TimelineRecord(t0: day1 + 6 * 600, kind: .event, label: "Contact state change",
                           family: .touch, zone: "zone:porch"),
            TimelineRecord(t0: day1 + 40 * 600, kind: .gap, label: "Storage full",
                           family: .gap, details: "disk at 98%"),
            TimelineRecord(t0: day1 + 41 * 600, kind: .heartbeat, label: "Heartbeat", family: .other),
            TimelineRecord(t0: day2 + 10 * 600, kind: .event, label: "Tamper detected",
                           family: .tamper, zone: "zone:gate"),
        ]
    }

    func testARoomShowsItsOwnNumberOfDaysOldestFirst() {
        let days = WallTimeline.days(for: twoDays, profile: .home, calendar: utcCalendar)
        XCTAssertEqual(days.count, 2)
        XCTAssertLessThan(days[0].dayT0, days[1].dayT0)
        XCTAssertEqual(days[0].cells.map(\.index), [6, 40], "a heartbeat lights no cell")
        XCTAssertEqual(WallTimeline.days(for: twoDays, profile: .apartment, calendar: utcCalendar).map(\.dayT0),
                       [days[1].dayT0], "the peephole shows today")
        XCTAssertEqual(WallTimeline.dayLimit(for: .business), 5)
    }

    func testTheRemoteStepsThroughLitBucketsAndDays() throws {
        let days = WallTimeline.days(for: twoDays, profile: .home, calendar: utcCalendar)
        let start = try XCTUnwrap(WallTimeline.initialSelection(days))
        XCTAssertEqual(start, .init(day: 1, cell: 10), "newest day, latest lit bucket")

        XCTAssertEqual(WallTimeline.step(start, .right, in: days), start, "stops at the row's end")
        let older = WallTimeline.step(start, .up, in: days)
        XCTAssertEqual(older, .init(day: 0, cell: 6), "the older day's lit bucket nearest 10")
        XCTAssertEqual(WallTimeline.step(older, .right, in: days), .init(day: 0, cell: 40),
                       "quiet buckets are skipped, not stepped through")
        XCTAssertEqual(WallTimeline.step(older, .left, in: days), older)
        XCTAssertEqual(WallTimeline.step(older, .up, in: days), older, "no day above the oldest")
        XCTAssertEqual(WallTimeline.step(.init(day: 9, cell: 0), .left, in: days), start,
                       "a cursor the tail moved past starts over")
        XCTAssertNil(WallTimeline.initialSelection([]))
    }

    func testABucketListsWhatItsCellCounted() {
        let days = WallTimeline.days(for: twoDays, profile: .home, calendar: utcCalendar)
        let gapBucket = WallTimeline.records(at: .init(day: 0, cell: 40), in: days, from: twoDays,
                                             bucketSeconds: 600, calendar: utcCalendar)
        XCTAssertEqual(gapBucket.map(\.label), ["Storage full"])
        XCTAssertEqual(WallTimeline.line(for: gapBucket[0]), "Storage full · disk at 98%")
        let heartbeatOnly = WallTimeline.records(at: .init(day: 0, cell: 41), in: days, from: twoDays,
                                                 bucketSeconds: 600, calendar: utcCalendar)
        XCTAssertTrue(heartbeatOnly.isEmpty, "proof of watching is not something that happened")
        let tamper = WallTimeline.records(at: .init(day: 1, cell: 10), in: days, from: twoDays,
                                          bucketSeconds: 600, calendar: utcCalendar)
        XCTAssertEqual(tamper.map { WallTimeline.line(for: $0) }, ["Tamper detected · zone:gate"])
    }

    func testCellsWearOnlyTheirReservedRoles() {
        let days = WallTimeline.days(for: twoDays, profile: .home, calendar: utcCalendar)
        XCTAssertEqual(days[0].cells.map { WallTimeline.role(for: $0) }, [.neutral, .warn],
                       "an ordinary event borrows no status color; a declared gap wears warn")
        XCTAssertEqual(days[1].cells.map { WallTimeline.role(for: $0) }, [.tamper])
    }

    func testABucketIsPrintedAsARangeNeverAnInstant() {
        let range = WallTimeline.bucketRange(start: 1_700_000_400, seconds: 600,
                                             timeZone: utc, locale: Locale(identifier: "en_US_POSIX"))
        XCTAssertTrue(range.contains("10:20"), range)
        XCTAssertTrue(range.contains("10:30"), range)
        XCTAssertTrue(range.contains(" – "), "a start and an end, always: \(range)")
    }

    func testTheDetailsStripPrintsTheSealedBucketNotTheCell() throws {
        // Asia/Kathmandu is UTC+5:45, so this TV's day starts off the sealed
        // ten-minute grid: the bucket sealed at 04:05 local lands in the
        // 04:00 cell. The strip must print 4:05 – 4:15, the window the
        // record holds — never the cell's 4:00 – 4:10, which it never sealed.
        let kathmandu = try XCTUnwrap(TimeZone(identifier: "Asia/Kathmandu"))
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = kathmandu
        let sealed = TimelineRecord(t0: 1_700_000_400, size: 600, kind: .event,
                                    label: "Contact state change", family: .touch)
        let days = WallTimeline.days(for: [sealed], profile: .home, calendar: calendar)
        let selection = try XCTUnwrap(WallTimeline.initialSelection(days))
        XCTAssertEqual(selection.cell, 24, "04:05 local sits in the 04:00 cell of this TV's grid")

        let rows = WallTimeline.records(at: selection, in: days, from: [sealed],
                                        bucketSeconds: 600, calendar: calendar)
        let buckets = WallTimeline.sealedBuckets(rows, fallbackSeconds: 600)
        XCTAssertEqual(buckets.map(\.start), [1_700_000_400])
        XCTAssertEqual(buckets.map(\.seconds), [600])
        let printed = WallTimeline.bucketRange(start: buckets[0].start, seconds: buckets[0].seconds,
                                               timeZone: kathmandu, locale: Locale(identifier: "en_US_POSIX"))
        XCTAssertTrue(printed.contains("4:05"), printed)
        XCTAssertTrue(printed.contains("4:15"), printed)
        XCTAssertFalse(printed.contains("4:00"), "the cell's start is not a time the record sealed: \(printed)")
    }

    func testACellHoldingTwoSealedBucketsPrintsBoth() {
        // A cell can hold more than one sealed bucket (a 25-hour day clamps
        // its last hour into one cell; a tail can mix bucket sizes). Each
        // record is headed by its own range, oldest first — never merged
        // into one window the record did not seal.
        let a = TimelineRecord(t0: 1_700_000_400, size: 600, kind: .event, label: "A", family: .touch)
        let b = TimelineRecord(t0: 1_700_000_400, size: 600, kind: .event, label: "B", family: .touch)
        let c = TimelineRecord(t0: 1_700_000_100, size: 300, kind: .event, label: "C", family: .touch)
        let buckets = WallTimeline.sealedBuckets([a, b, c], fallbackSeconds: 600)
        XCTAssertEqual(buckets.map(\.start), [1_700_000_100, 1_700_000_400])
        XCTAssertEqual(buckets.map(\.seconds), [300, 600])
        XCTAssertEqual(buckets.map { $0.records.map(\.label) }, [["C"], ["A", "B"]])
        XCTAssertTrue(WallTimeline.sealedBuckets([], fallbackSeconds: 600).isEmpty)
    }
}
