// HeadWatchTests.swift
//
// The sentinel's event eyes, held still: first sight is a baseline (pairing
// a Canary with a long chain is not four thousand fresh events), any later
// movement is news — including backwards, because a chain that restarted is
// exactly the kind of thing worth a full refresh — and an unpaired device
// leaves no stale baseline behind.

import XCTest
@testable import SecuraCV

final class HeadWatchTests: XCTestCase {
    func testFirstSightIsABaselineNotNews() {
        var watch = HeadWatch()
        XCTAssertFalse(watch.hasNews(id: "a", headSeq: 4000))
    }

    func testAnAdvancedHeadIsNews() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 10)
        XCTAssertTrue(watch.hasNews(id: "a", headSeq: 11))
    }

    func testAStillHeadIsQuiet() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 10)
        XCTAssertFalse(watch.hasNews(id: "a", headSeq: 10))
        XCTAssertFalse(watch.hasNews(id: "a", headSeq: 10))
    }

    func testABackwardsHeadIsNewsToo() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 500)
        XCTAssertTrue(watch.hasNews(id: "a", headSeq: 3),
                      "a chain that restarted deserves a refresh and a verify, not a shrug")
    }

    func testNewsMovesTheBaseline() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 10)
        _ = watch.hasNews(id: "a", headSeq: 11)
        XCTAssertFalse(watch.hasNews(id: "a", headSeq: 11),
                       "the same news must not fire twice")
    }

    func testDevicesAreWatchedIndependently() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 10)
        _ = watch.hasNews(id: "b", headSeq: 99)
        XCTAssertTrue(watch.hasNews(id: "a", headSeq: 11))
        XCTAssertFalse(watch.hasNews(id: "b", headSeq: 99))
    }

    func testForgettingADeviceResetsItsBaseline() {
        var watch = HeadWatch()
        _ = watch.hasNews(id: "a", headSeq: 10)
        watch.forget(id: "a")
        XCTAssertFalse(watch.hasNews(id: "a", headSeq: 999),
                       "a re-paired device starts from a baseline, not from stale memory")
    }
}
