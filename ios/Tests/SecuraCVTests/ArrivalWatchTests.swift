// ArrivalWatchTests.swift
//
// "New since you opened this" (ArrivalWatch.swift): what was on the network
// when the walkthrough opened is the baseline; only a later same-kind Canary
// is new. The trap these pin is a late browse — Local Network still being
// asked for, or a browse restarted after the Camera app — where devices that
// were always there arrive late and must not look new.

import XCTest
@testable import SecuraCV

final class ArrivalWatchTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_800_000_000)
    private func at(_ s: TimeInterval) -> Date { t0.addingTimeInterval(s) }

    func testWhatWasAlreadyThereIsNeverNew() {
        var w = ArrivalWatch()
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "dash-b"], at: at(0)), [])
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "dash-b"], at: at(60)), [])
    }

    func testAOneThatJoinsLaterIsNew() {
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(0))
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "glance-k7"], at: at(45)), ["glance-k7"])
    }

    func testSlowAnswersInsideTheSettleWindowCountAsAlreadyThere() {
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: [], at: at(0))
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(4))  // mDNS answered late
        XCTAssertFalse(w.settled(at: at(4)))
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a"], at: at(30)), [])
        XCTAssertTrue(w.settled(at: at(30)))
    }

    func testTheSettleWindowStartsWhenTheBrowseRunsNotWhenTheScreenOpened() {
        // The Local Network prompt sat for a minute; then the browse ran and
        // the house's existing display answered at once.
        var w = ArrivalWatch()
        XCTAssertEqual(w.observe(browsing: false, ids: [], at: at(0)), [])
        XCTAssertEqual(w.observe(browsing: false, ids: [], at: at(60)), [])
        XCTAssertNil(w.windowStart, "nothing settles while the browse is down")
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a"], at: at(61)), [],
                       "the old display is baseline, not new")
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "glance-k7"], at: at(120)), ["glance-k7"])
    }

    func testAWindowCutShortByTheBackgroundStartsOver() {
        // Review on the first cut: browse runs empty at 0, the person jumps
        // to the Camera app at 1, and on return at 60 the display they
        // already own answers. Background time must not complete the window.
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: [], at: at(0))
        _ = w.observe(browsing: false, ids: [], at: at(1))
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a"], at: at(60)), [],
                       "the owned display answering on return is baseline")
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a"], at: at(65)), [])
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "glance-k7"], at: at(90)), ["glance-k7"])
    }

    func testLeavingTheForegroundInterruptsEvenWhenTheStopIsNeverSeen() {
        // The stop and restart can land between two renders, so the view
        // also calls interrupt() when the scene leaves .active.
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: [], at: at(0))
        w.interrupt()
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a"], at: at(60)), [])
        XCTAssertFalse(w.settled(at: at(60)))
        XCTAssertTrue(w.settled(at: at(71)))
    }

    func testAnInterruptAfterSettlingChangesNothing() {
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(0))
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(15))
        XCTAssertTrue(w.isSettled)
        w.interrupt()
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "glance-k7"], at: at(80)), ["glance-k7"])
    }

    func testABrowseThatDropsAndComesBackKeepsItsBaseline() {
        // The Camera-app route: the app goes to the background, the browse
        // stops, and on return it hears everyone again.
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(0))
        _ = w.observe(browsing: true, ids: ["dash-a"], at: at(20))
        XCTAssertEqual(w.observe(browsing: false, ids: [], at: at(30)), [])
        XCTAssertEqual(w.observe(browsing: true, ids: ["dash-a", "glance-k7"], at: at(90)), ["glance-k7"],
                       "the returning old one is still baseline; only the new one is new")
    }

    func testNewcomersKeepTheOrderTheyWereHeardIn() {
        var w = ArrivalWatch()
        _ = w.observe(browsing: true, ids: ["a"], at: at(0))
        XCTAssertEqual(w.observe(browsing: true, ids: ["c", "a", "b"], at: at(40)), ["c", "b"])
    }
}
