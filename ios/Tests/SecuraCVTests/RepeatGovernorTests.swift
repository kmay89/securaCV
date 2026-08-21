// RepeatGovernorTests.swift
//
// The dog-at-the-door damper, held still: the first alert of a burst buzzes,
// repeats earn doubling rests, a real escalation pierces, tamper and a failed
// chain are never governed, and a calm half hour makes the next alert news
// again. Plus the storm collapse: three simultaneous alerts become one
// summary that names the count and the worst thing happening.

import XCTest
@testable import SecuraCV

final class RepeatGovernorTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    private func verdict(severity: Severity = .alert,
                         tamper: Bool = false,
                         integrityFailed: Bool = false,
                         memory: RepeatGovernor.Memory?,
                         at date: Date) -> RepeatGovernor.Verdict {
        RepeatGovernor.consider(severity: severity, tamper: tamper,
                                integrityFailed: integrityFailed,
                                memory: memory, now: date)
    }

    func testFirstAlertOfABurstBuzzes() {
        let v = verdict(memory: nil, at: t0)
        XCTAssertTrue(v.buzz)
        XCTAssertEqual(v.memory.buzzCount, 1)
        XCTAssertNil(v.restingFor)
    }

    func testAQuickRepeatRests() {
        let first = verdict(memory: nil, at: t0)
        let repeatV = verdict(memory: first.memory, at: t0.addingTimeInterval(30))
        XCTAssertFalse(repeatV.buzz)
        // 120 s cooldown, 30 s elapsed → 90 s left.
        XCTAssertEqual(repeatV.restingFor, 90)
        // A rested repeat must not advance the burst — the rest it earned
        // stays the rest it earned.
        XCTAssertEqual(repeatV.memory.buzzCount, 1)
    }

    func testRestsDoubleUpToTheCap() {
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 1), 120)
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 2), 240)
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 3), 480)
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 4), 960)
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 5), 1800)
        // The ceiling holds no matter how long the dog barks.
        XCTAssertEqual(RepeatGovernor.cooldown(afterBuzzes: 40), 1800)
    }

    func testARepeatPastItsRestBuzzesAgain() {
        let first = verdict(memory: nil, at: t0)
        let later = verdict(memory: first.memory, at: t0.addingTimeInterval(121))
        XCTAssertTrue(later.buzz)
        XCTAssertEqual(later.memory.buzzCount, 2)
    }

    func testEscalationPiercesTheRest() {
        let opening = verdict(severity: .warn, memory: nil, at: t0)
        let escalated = verdict(severity: .alert, memory: opening.memory,
                                at: t0.addingTimeInterval(10))
        XCTAssertTrue(escalated.buzz, "a worse condition is news no matter how recent the last buzz was")
        XCTAssertEqual(escalated.memory.worstBuzzed, .alert)
    }

    func testTamperIsNeverGoverned() {
        var memory: RepeatGovernor.Memory?
        // Exhaust the burst hard.
        for i in 0..<5 {
            let v = verdict(memory: memory, at: t0.addingTimeInterval(Double(i) * 3600))
            memory = v.memory
        }
        let tamper = verdict(severity: .tamper, tamper: true, memory: memory,
                             at: t0.addingTimeInterval(5 * 3600 + 1))
        XCTAssertTrue(tamper.buzz)
    }

    func testAFailedChainIsNeverGoverned() {
        let first = verdict(memory: nil, at: t0)
        let failed = verdict(severity: .alert, integrityFailed: true,
                             memory: first.memory, at: t0.addingTimeInterval(1))
        XCTAssertTrue(failed.buzz)
    }

    func testACalmGapEndsTheBurst() {
        let first = verdict(memory: nil, at: t0)
        let second = verdict(memory: first.memory, at: t0.addingTimeInterval(121))
        // Long quiet, then news — a fresh story with a fresh burst.
        let fresh = verdict(memory: second.memory,
                            at: t0.addingTimeInterval(121 + RepeatGovernor.calmGap))
        XCTAssertTrue(fresh.buzz)
        XCTAssertEqual(fresh.memory.buzzCount, 1, "a calm gap resets the doubling")
    }

    func testContinuousFlappingNeverReadsAsCalm() {
        // A Canary flapping every 10 minutes for over an hour. Under the
        // 30-minute cooldown ceiling more than 30 minutes can pass between
        // BUZZES while the condition never once goes quiet — that must not
        // read as a calm gap and hand the short cooldowns back.
        var memory: RepeatGovernor.Memory?
        var buzzCounts: [Int] = []
        for minute in stride(from: 0, through: 80, by: 10) {
            let v = verdict(memory: memory,
                            at: t0.addingTimeInterval(Double(minute) * 60))
            memory = v.memory
            if v.buzz { buzzCounts.append(v.memory.buzzCount) }
        }
        // Buzzes land at 0, 10, 20, 30, 50 and 80 minutes (rests doubling
        // between them); the burst count only ever climbs — a reset to 1
        // would be the fake-calm bug this test pins down.
        XCTAssertEqual(buzzCounts, [1, 2, 3, 4, 5, 6])
    }

    func testARestedRepeatAdvancesTheSeenClockOnly() {
        let first = verdict(memory: nil, at: t0)
        let rested = verdict(memory: first.memory, at: t0.addingTimeInterval(30))
        XCTAssertEqual(rested.memory.lastSeenAt, t0.addingTimeInterval(30))
        XCTAssertEqual(rested.memory.lastBuzzAt, t0, "a rest is seen, never counted as a buzz")
        XCTAssertEqual(rested.memory.buzzCount, 1)
    }

    func testSameSeverityRepeatDoesNotPierce() {
        // The dog case exactly: alert → alert with a different status line.
        let first = verdict(severity: .alert, memory: nil, at: t0)
        let flap = verdict(severity: .alert, memory: first.memory,
                           at: t0.addingTimeInterval(15))
        XCTAssertFalse(flap.buzz)
    }

    func testRestingReasonSpeaksMinutes() {
        XCTAssertEqual(RepeatGovernor.restingReason(90),
                       "Grouped with the last alert from this Canary — repeats rest for 2 min.")
        XCTAssertEqual(RepeatGovernor.restingReason(30),
                       "Grouped with the last alert from this Canary — repeats rest for 1 min.")
    }

    // MARK: - the storm collapse

    private func pending(_ name: String, _ sev: Severity,
                         _ line: String = "Gone dark") -> AlertStorm.Pending {
        AlertStorm.Pending(name: name, severity: sev, statusLine: line)
    }

    func testBelowTheThresholdNothingCollapses() {
        XCTAssertNil(AlertStorm.collapse([pending("Porch", .alert),
                                          pending("Garage", .alert)]))
    }

    func testAtTheThresholdOneSummaryNamesCountAndWorst() {
        let summary = AlertStorm.collapse([
            pending("Porch", .alert),
            pending("Garage", .tamper, "Tamper detected"),
            pending("Shed", .alert),
        ])
        XCTAssertNotNil(summary)
        XCTAssertEqual(summary?.worst, .tamper)
        XCTAssertEqual(summary?.body,
                       "3 Canaries need attention. Worst: Garage — Tamper detected")
    }

    func testAnEmptyPassCollapsesToNothing() {
        XCTAssertNil(AlertStorm.collapse([]))
    }
}
