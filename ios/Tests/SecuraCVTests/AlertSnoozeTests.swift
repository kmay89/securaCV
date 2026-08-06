// AlertSnoozeTests.swift
//
// The mute durations, pinned. Two properties matter more than the arithmetic:
// every duration ENDS (there is no untimed case to construct), and no offered
// choice ever means something wildly longer than its label — "until tonight"
// tapped at 10pm must not quietly buy 23 hours of silence.

import XCTest
@testable import SecuraCV

final class AlertSnoozeTests: XCTestCase {
    /// A fixed calendar so "9pm" means 9pm wherever this runs.
    private var calendar: Calendar {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "America/New_York")!
        return cal
    }

    private func at(_ hour: Int, _ minute: Int = 0, day: Int = 14) throws -> Date {
        let comps = DateComponents(year: 2026, month: 8, day: day, hour: hour, minute: minute)
        return try XCTUnwrap(calendar.date(from: comps))
    }

    func testEveryDurationEnds() throws {
        let now = try at(14)
        for duration in MuteDuration.allCases {
            XCTAssertGreaterThan(duration.expiry(from: now, calendar: calendar), now,
                                 "\(duration.rawValue) must end — an untimed mute is unrepresentable here")
        }
    }

    func testOneHourIsAnHour() throws {
        let now = try at(14, 30)
        XCTAssertEqual(MuteDuration.oneHour.expiry(from: now, calendar: calendar),
                       now.addingTimeInterval(3600))
    }

    func testUntilTonightRunsToThisEvening() throws {
        let now = try at(14, 30)
        XCTAssertEqual(MuteDuration.untilTonight.expiry(from: now, calendar: calendar),
                       try at(21))
    }

    func testUntilMorningCrossesMidnight() throws {
        let now = try at(23, 15)
        XCTAssertEqual(MuteDuration.untilMorning.expiry(from: now, calendar: calendar),
                       try at(7, 0, day: 15), "11pm means TOMORROW morning")
    }

    func testUntilMorningBeforeDawnIsTodays() throws {
        let now = try at(3)
        XCTAssertEqual(MuteDuration.untilMorning.expiry(from: now, calendar: calendar),
                       try at(7), "3am means this morning, four hours out — not tomorrow's")
    }

    // MARK: - the menu never offers a mislabeled length

    func testEveningChoiceIsNotOfferedAtNight() throws {
        let offered = MuteDuration.offered(at: try at(22), calendar: calendar)
        XCTAssertFalse(offered.contains(.untilTonight),
                       "‘until tonight’ at 10pm would mean tomorrow night — a day of silence wearing an evening's name")
        XCTAssertTrue(offered.contains(.untilMorning))
    }

    func testMorningChoiceIsNotOfferedInTheMorning() throws {
        let offered = MuteDuration.offered(at: try at(9), calendar: calendar)
        XCTAssertFalse(offered.contains(.untilMorning),
                       "‘until morning’ at 9am would be a 22-hour mute")
        XCTAssertTrue(offered.contains(.untilTonight))
    }

    func testAnHourIsAlwaysOffered() throws {
        for hour in 0..<24 {
            XCTAssertTrue(MuteDuration.offered(at: try at(hour), calendar: calendar).contains(.oneHour),
                          "hour \(hour) lost the one choice that always makes sense")
        }
    }
}
