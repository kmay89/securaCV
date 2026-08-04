// GlanceAnswerTests.swift
//
// The spoken answer, held to the same honesty rules as every glance surface:
// an old snapshot says its age, a dark delivery path outranks green rows,
// sample data is labeled in the sentence itself, and the quiet-hour copy
// names the punch-through every single time. Pure Foundation with injected
// clocks — the intents render these strings; the strings are the contract.

import XCTest
@testable import SecuraCV

final class GlanceAnswerTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_784_000_000)

    /// A fresh, real (non-demo) snapshot to mutate per test.
    private func snapshot(severity: Severity = .ok,
                          headline: String = "All quiet",
                          heartbeat: WristHeartbeatState = .alive,
                          sentAgo: TimeInterval = 120,
                          demo: Bool = false) -> WristSnapshot {
        var snap = WristSnapshot.sample(now: now)
        snap.severityRaw = severity.rawValue
        snap.headline = headline
        snap.heartbeatRaw = heartbeat.rawValue
        snap.sentAt = now.addingTimeInterval(-sentAgo)
        snap.isDemoData = demo
        return snap
    }

    // MARK: - the one honest answer

    func testNoSnapshotInvitesTheFirstOpen() {
        let answer = GlanceAnswer.spoken(nil, now: now)
        XCTAssertTrue(answer.contains("open the app once"), answer)
    }

    func testAQuietFleetSaysAllsWellWithTheCount() {
        let answer = GlanceAnswer.spoken(snapshot(), now: now)
        XCTAssertTrue(answer.hasPrefix("All's well"), answer)
        XCTAssertTrue(answer.contains("3 of 3 healthy"), answer)
    }

    func testTroubleLeadsWithTheHeadlineNotTheCount() {
        let snap = snapshot(severity: .alert, headline: "Front Porch • Tamper")
        let answer = GlanceAnswer.spoken(snap, now: now)
        XCTAssertTrue(answer.hasPrefix("Front Porch: Tamper"), answer)
        // The glass separator never reaches a spoken sentence.
        XCTAssertFalse(answer.contains("•"), answer)
    }

    func testAnOldSnapshotSaysItsAge() {
        let stale = GlanceAnswer.spoken(snapshot(sentAgo: 40 * 60), now: now)
        XCTAssertTrue(stale.hasPrefix("As of 40 minutes ago:"), stale)

        let hours = GlanceAnswer.spoken(snapshot(sentAgo: 3 * 3600), now: now)
        XCTAssertTrue(hours.hasPrefix("As of about 3 hours ago:"), hours)

        let fresh = GlanceAnswer.spoken(snapshot(sentAgo: 120), now: now)
        XCTAssertFalse(fresh.contains("As of"), fresh)
    }

    func testSampleDataIsLabeledInTheSentenceItself() {
        let demo = GlanceAnswer.spoken(snapshot(demo: true), now: now)
        XCTAssertTrue(demo.contains("Sample data."), demo)

        let real = GlanceAnswer.spoken(snapshot(demo: false), now: now)
        XCTAssertFalse(real.contains("Sample data"), real)
    }

    func testAQuietFleetWithADarkPathRefusesFalseComfort() {
        var snap = snapshot(heartbeat: .dark)
        snap.lastVerifiedAt = now.addingTimeInterval(-47 * 60)
        let answer = GlanceAnswer.spoken(snap, now: now)
        XCTAssertTrue(answer.contains("All's well"), answer)
        XCTAssertTrue(answer.contains("No heartbeat for 47 min"), answer)
    }

    func testAHealthyHeartbeatStaysOutOfAQuietAnswer() {
        let answer = GlanceAnswer.spoken(snapshot(heartbeat: .alive), now: now)
        XCTAssertFalse(answer.contains("heartbeat"), answer)
    }

    // MARK: - the quiet hour

    func testQuietHourCopyAlwaysNamesThePunchThrough() {
        let answer = GlanceAnswer.quieted(count: 5)
        XCTAssertTrue(answer.contains("all 5 Canaries"), answer)
        XCTAssertTrue(answer.contains("Tamper"), answer)
        XCTAssertTrue(answer.contains("signature failures"), answer)
    }

    func testQuietHourWithOneCanaryReadsNaturally() {
        let answer = GlanceAnswer.quieted(count: 1)
        XCTAssertTrue(answer.contains("your Canary"), answer)
        XCTAssertTrue(answer.contains("Tamper"), answer)
    }

    func testQuietHourWithNothingPairedIsHonest() {
        XCTAssertTrue(GlanceAnswer.quieted(count: 0).contains("Nothing to quiet"))
    }

    func testResumeCopyDistinguishesClearedFromAlreadyLoud() {
        XCTAssertTrue(GlanceAnswer.resumed(count: 3).contains("full volume"))
        XCTAssertTrue(GlanceAnswer.resumed(count: 0).contains("Nothing was muted"))
    }

    // MARK: - the path test verdict

    func testAVerifiedPathTestSaysTheFleetCanReachYou() {
        let verdict = GlanceAnswer.pathTest(verified: true,
                                            summary: "Delivery verified just now")
        XCTAssertEqual(verdict, "Your fleet can reach you — delivery verified just now.")
    }

    func testAFailedPathTestPassesTheSystemsObjectionThrough() {
        let verdict = GlanceAnswer.pathTest(verified: false,
                                            summary: "Test failed: notifications are off for SecuraCV.")
        XCTAssertEqual(verdict, "Test failed: notifications are off for SecuraCV.")
        // No double period when the reason already ends with one.
        XCTAssertFalse(verdict.hasSuffix(".."), verdict)
    }
}
