// DemoFleetTests.swift
//
// The demo fleet's ground rules, held by test so sample data can never
// impersonate a real fleet or fake an alarm (see DemoFleet.swift's header).

import XCTest
@testable import SecuraCV

final class DemoFleetTests: XCTestCase {
    // A fixed clock: the seed must be a pure function of `now`.
    let now = Date(timeIntervalSince1970: 1_784_000_000)

    func testEveryDemoIDIsNamespaced() {
        for w in DemoFleet.witnesses(now: now) {
            XCTAssertTrue(w.id.hasPrefix("demo-"), "witness id \(w.id) must be demo-namespaced")
        }
        for e in DemoFleet.timeline(now: now) {
            XCTAssertTrue(e.deviceID.hasPrefix("demo-"), "event device \(e.deviceID) must be demo-namespaced")
        }
    }

    func testDemoNeverFakesAnAlarm() {
        // Nothing seeded may cross .notice, carry a failed badge, or claim
        // tamper: the demo must never trip AlertCenter (which fires at
        // >= .alert) or wear the colors reserved for a real alarm.
        for w in DemoFleet.witnesses(now: now) {
            XCTAssertLessThanOrEqual(w.effectiveSeverity, .notice, w.id)
            XCTAssertNotEqual(w.badge, .failed, w.id)
            XCTAssertFalse(w.tamper, w.id)
        }
        for e in DemoFleet.timeline(now: now) {
            XCTAssertLessThanOrEqual(e.severity, .notice, e.id)
            XCTAssertNotEqual(e.badge, .failed, e.id)
        }
    }

    func testTimelineHonorsCoarseBuckets() {
        for e in DemoFleet.timeline(now: now) {
            XCTAssertEqual(
                e.timeBucket.timeIntervalSince1970.truncatingRemainder(dividingBy: 600), 0,
                "\(e.id) is not on a 10-minute boundary (Invariant III)")
        }
    }

    func testSeedIsDeterministicForAFixedClock() {
        XCTAssertEqual(DemoFleet.witnesses(now: now), DemoFleet.witnesses(now: now))
        XCTAssertEqual(DemoFleet.timeline(now: now), DemoFleet.timeline(now: now))
    }
}
