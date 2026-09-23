// TimelineCellBucketsTests.swift
//
// What time a ribbon cell PRINTS — the phone's port of the Witness Wall's
// review fix (tvos WallTimelineTests: "the details strip prints the sealed
// bucket, not the cell"). The ribbon cuts the phone's day at LOCAL midnight;
// the records hold buckets on the epoch grid. Where those two grids part —
// a UTC offset that is not a multiple of the bucket, a 25-hour day — the
// cell's own slot is a window no record holds, and it must never be printed
// for a lit cell. An empty cell holds no record, so its slot is all it has.
//
// Every vector here was checked against the IANA database (python zoneinfo)
// when it was written; the arithmetic is in each test's comment.

import XCTest
@testable import SecuraCV

final class TimelineCellBucketsTests: XCTestCase {

    private func calendar(_ identifier: String) throws -> Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = try XCTUnwrap(TimeZone(identifier: identifier))
        return calendar
    }

    /// A fixed, locale-independent clock in `calendar`'s zone, so the printed
    /// range does not depend on the simulator's settings.
    private func style(_ calendar: Calendar) -> Date.FormatStyle {
        Date.FormatStyle(date: .omitted, time: .shortened,
                         locale: Locale(identifier: "en_US_POSIX"),
                         calendar: calendar, timeZone: calendar.timeZone)
    }

    private func record(_ t0: Int, size: Int = TimelineScrub.defaultBucketSeconds,
                        _ label: String = "Contact state change") -> TimelineRecord {
        TimelineRecord(t0: t0, size: size, kind: .event, label: label, family: .touch)
    }

    /// The cells the model lights and the cells that get a label are the
    /// same cells — the placement rule is written twice, so pin it.
    private func assertSameCells(_ records: [TimelineRecord], bucketSeconds: Int, calendar: Calendar,
                                 file: StaticString = #filePath, line: UInt = #line) {
        let days = TimelineScrub.days(for: TimelineScrub.sorted(records), bucketSeconds: bucketSeconds,
                                      calendar: calendar)
        let byCell = TimelineCellBuckets.byCell(records, bucketSeconds: bucketSeconds, calendar: calendar)
        XCTAssertEqual(Set(byCell.keys), Set(days.map(\.dayT0)), "same days", file: file, line: line)
        for day in days {
            let lit = byCell[day.dayT0].map { Array($0.keys) } ?? []
            XCTAssertEqual(Set(lit), Set(day.cells.map(\.index)),
                           "same lit cells on \(day.label)", file: file, line: line)
        }
    }

    func testAPlusFiveFortyFiveZonePrintsTheRecordsBucketNotTheCell() throws {
        // Asia/Kathmandu is UTC+5:45, so the phone's day starts off the
        // ten-minute epoch grid: 1_700_000_400 is 2023-11-15 04:05 +05:45,
        // local midnight is 1_699_985_700, and the record lands in cell 24 —
        // the 04:00 slot. The ribbon must print 4:05 – 4:15, the window the
        // record holds, never the slot's 4:00 – 4:10.
        let kathmandu = try calendar("Asia/Kathmandu")
        let alert = record(1_700_000_400)
        assertSameCells([alert], bucketSeconds: 600, calendar: kathmandu)

        let dayT0 = 1_699_985_700
        let cells = try XCTUnwrap(TimelineCellBuckets.byCell([alert], bucketSeconds: 600,
                                                             calendar: kathmandu)[dayT0])
        XCTAssertEqual(Array(cells.keys), [24], "04:05 local sits in the 04:00 cell")
        XCTAssertEqual(cells[24], [TimelineCellBuckets.Bucket(start: 1_700_000_400, seconds: 600)])

        let printed = TimelineCellBuckets.label(forCell: 24, dayT0: dayT0, cells: cells,
                                                style: style(kathmandu))
        XCTAssertTrue(printed.contains("4:05"), printed)
        XCTAssertTrue(printed.contains("4:15"), printed)
        XCTAssertFalse(printed.contains("4:00"), "the slot's start is not a time the record holds: \(printed)")

        // An empty cell holds no record, so it prints its own slot.
        let empty = TimelineCellBuckets.label(forCell: 23, dayT0: dayT0, cells: cells, style: style(kathmandu))
        XCTAssertTrue(empty.contains("3:50") && empty.contains("4:00"), empty)

        // And the list walks to the record: the hosts scroll to the first row
        // at or before the target, and the slot's start (04:00) fell before
        // the 04:05 row — the list landed on the row behind it.
        let slot = Date(timeIntervalSince1970: TimeInterval(dayT0 + 24 * 600))
        XCTAssertEqual(TimelineCellBuckets.scrollTarget(seated: slot, cell: cells[24]),
                       Date(timeIntervalSince1970: 1_700_000_400))
        XCTAssertEqual(TimelineCellBuckets.scrollTarget(seated: slot, cell: nil), slot,
                       "an empty cell scrolls by its own slot")
    }

    func testAPlusFiveThirtyZoneWithHourBucketsPrintsTheRecordsHour() throws {
        // Asia/Kolkata is UTC+5:30: on the hour grid, 1_699_999_200 (a whole
        // epoch hour) is 2023-11-15 03:30 +05:30; local midnight is
        // 1_699_986_600, so the record lands in cell 3 — the 03:00 hour. It
        // must print 3:30 – 4:30, never 3:00.
        let kolkata = try calendar("Asia/Kolkata")
        let hour = record(1_699_999_200, size: 3600)
        assertSameCells([hour], bucketSeconds: 3600, calendar: kolkata)

        let cells = try XCTUnwrap(TimelineCellBuckets.byCell([hour], bucketSeconds: 3600,
                                                             calendar: kolkata)[1_699_986_600])
        XCTAssertEqual(cells[3], [TimelineCellBuckets.Bucket(start: 1_699_999_200, seconds: 3600)])
        let printed = TimelineCellBuckets.label(forCell: 3, dayT0: 1_699_986_600, cells: cells,
                                                gridSeconds: 3600, style: style(kolkata))
        XCTAssertTrue(printed.contains("3:30"), printed)
        XCTAssertTrue(printed.contains("4:30"), printed)
        XCTAssertFalse(printed.contains("3:00"), "the slot's start is not a time the record holds: \(printed)")
    }

    func testAnAlignedZoneReadsExactlyAsBefore() throws {
        // +5:30 IS a multiple of ten minutes, so with the phone's own buckets
        // the grids agree: 1_700_000_400 is 03:50 +05:30, cell 23, and the
        // record's bucket is the slot. The port changes nothing there — not
        // the words, not where the list lands.
        let kolkata = try calendar("Asia/Kolkata")
        let alert = record(1_700_000_400)
        let dayT0 = 1_699_986_600
        let cells = try XCTUnwrap(TimelineCellBuckets.byCell([alert], bucketSeconds: 600,
                                                             calendar: kolkata)[dayT0])
        XCTAssertEqual(Array(cells.keys), [23])
        let slotStart = dayT0 + 23 * 600
        XCTAssertEqual(slotStart, alert.t0)
        XCTAssertEqual(TimelineCellBuckets.label(forCell: 23, dayT0: dayT0, cells: cells, style: style(kolkata)),
                       TimelineCellBuckets.range(start: slotStart, seconds: 600, style: style(kolkata)))
        let slot = Date(timeIntervalSince1970: TimeInterval(slotStart))
        XCTAssertEqual(TimelineCellBuckets.scrollTarget(seated: slot, cell: cells[23]), slot)
    }

    func testATwentyFiveHourDayPrintsEveryBucketItsLastCellHolds() throws {
        // America/New_York falls back on 2023-11-05: the day starts at
        // 1_699_156_800 (00:00 EDT) and runs 25 hours. The strip holds 24, so
        // everything after 22:50 EST clamps into cell 143 — here the 22:50,
        // 23:00 and 23:40 EST buckets. The slot names only 10:50 – 11:00 PM;
        // the cell must name all three, oldest first, and walk the list to
        // the newest.
        let newYork = try calendar("America/New_York")
        let dayT0 = 1_699_156_800
        let late = [record(1_699_245_600, "C"), record(1_699_242_600, "A"),
                    record(1_699_243_200, "B"), record(1_699_245_600, "D")]
        assertSameCells(late, bucketSeconds: 600, calendar: newYork)

        let cells = try XCTUnwrap(TimelineCellBuckets.byCell(late, bucketSeconds: 600,
                                                             calendar: newYork)[dayT0])
        XCTAssertEqual(Array(cells.keys), [143], "the 25th hour clamps into the last cell")
        let last = try XCTUnwrap(cells[143])
        XCTAssertEqual(last.map(\.start), [1_699_242_600, 1_699_243_200, 1_699_245_600],
                       "each bucket once, oldest first — two records in one bucket print it once")

        let printed = TimelineCellBuckets.label(forCell: 143, dayT0: dayT0, cells: cells, style: style(newYork))
        XCTAssertEqual(printed.components(separatedBy: ", ").count, 3, printed)
        for time in ["10:50", "11:00", "11:10", "11:40", "11:50"] {
            XCTAssertTrue(printed.contains(time), "\(time) missing from \(printed)")
        }
        let slot = Date(timeIntervalSince1970: TimeInterval(dayT0 + 143 * 600))
        XCTAssertEqual(TimelineCellBuckets.scrollTarget(seated: slot, cell: cells[143]),
                       Date(timeIntervalSince1970: 1_699_245_600))
    }

    func testBucketsAreEachRecordsOwnNeverMerged() {
        // The Wall's companion case: mixed sizes in one cell each print their
        // own range, oldest first; a size that is not positive falls back.
        let buckets = TimelineCellBuckets.buckets(of: [record(1_700_000_400), record(1_700_000_100, size: 300),
                                                       record(1_700_000_400), record(1_700_000_700, size: 0)],
                                                  fallbackSeconds: 600)
        XCTAssertEqual(buckets.map(\.start), [1_700_000_100, 1_700_000_400, 1_700_000_700])
        XCTAssertEqual(buckets.map(\.seconds), [300, 600, 600])
        XCTAssertTrue(TimelineCellBuckets.buckets(of: [], fallbackSeconds: 600).isEmpty)

        // Heartbeats light no cell (TimelineScrub.days), so they name none.
        let beat = TimelineRecord(t0: 1_700_000_400, kind: .heartbeat, label: "Heartbeat", family: .other)
        XCTAssertTrue(TimelineCellBuckets.byCell([beat], bucketSeconds: 600,
                                                 calendar: TimelineScrub.utcCalendar).isEmpty)
    }
}
