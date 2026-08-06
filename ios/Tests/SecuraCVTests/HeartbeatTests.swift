// HeartbeatTests.swift
//
// The dead-man's-switch, and the one thing it must never do: cry wolf. A
// false "your fleet went dark" is how a real one gets ignored, so silence
// only counts when the app could actually hear (it hears nothing in the
// background by design) and when there was something to hear at all.
//
// The other pinned promise is the honesty split: a Canary answering on the
// LAN is NOT proof that a notification would reach a phone across town, and
// the copy must never let the weaker claim borrow the stronger one's words.

import XCTest
@testable import SecuraCV

@MainActor
final class HeartbeatTests: XCTestCase {
    private func freshDefaults() throws -> (UserDefaults, String) {
        let suite = "test-heartbeat-\(UUID().uuidString)"
        return (try XCTUnwrap(UserDefaults(suiteName: suite)), suite)
    }

    private let t0 = Date(timeIntervalSince1970: 1_800_000_000)

    // MARK: - a verification the user earned survives a relaunch

    func testTheLastVerificationPersists() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let first = Heartbeat(defaults: defaults)
        first.recordBeat(source: .pathVerified, now: t0)

        let relaunched = Heartbeat(defaults: defaults)
        XCTAssertEqual(relaunched.lastVerified, t0,
                       "a cold start used to throw away the proof the user ran a test for")
        XCTAssertEqual(relaunched.lastBeat, t0)
        XCTAssertEqual(relaunched.lastBeatSource, .pathVerified)
    }

    func testResetForgetsEverythingIncludingOnDisk() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.recordBeat(source: .pathVerified, now: t0)
        beat.reset()
        XCTAssertNil(Heartbeat(defaults: defaults).lastVerified)
    }

    // MARK: - the honesty split

    func testAFleetCheckInNeverClaimsAVerifiedDelivery() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.recordBeat(source: .fleetCheckIn, now: t0)
        XCTAssertNil(beat.lastVerified,
                     "a Canary answering is not evidence that a notification would land")
        XCTAssertTrue(beat.summary.hasPrefix("Your fleet checked in"), beat.summary)
    }

    func testAnAcceptedDeliveryIsAVerification() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.recordBeat(source: .pathVerified, now: t0)
        XCTAssertEqual(beat.lastVerified, t0)
        XCTAssertTrue(beat.summary.hasPrefix("Delivery verified"), beat.summary)
    }

    // MARK: - silence is only evidence when we were listening

    func testBackgroundedTimeIsNotEvidenceOfASilentFleet() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.expectsBeats = true
        beat.recordBeat(source: .fleetCheckIn, now: t0)

        // 40 minutes with the app closed — the radios were off, so this
        // proves nothing about the fleet.
        let returned = t0.addingTimeInterval(2400)
        beat.noteListening(now: returned)
        beat.tick(now: returned.addingTimeInterval(60))
        XCTAssertTrue(beat.state.isHealthy,
                      "the app can't hear in the background; accusing the fleet for that would be crying wolf")
    }

    func testRealSilenceStillGoesDark() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.expectsBeats = true
        beat.noteListening(now: t0)
        beat.recordBeat(source: .fleetCheckIn, now: t0)
        beat.tick(now: t0.addingTimeInterval(2400))   // listening the whole time
        guard case .dark = beat.state else {
            return XCTFail("a fleet that stops answering while we're listening IS the alarm: \(beat.state)")
        }
    }

    func testNothingPairedMeansNoDeadMansSwitch() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.expectsBeats = false                     // no paired devices
        beat.noteListening(now: t0)
        beat.recordBeat(source: .pathVerified, now: t0)
        beat.tick(now: t0.addingTimeInterval(86_400))
        if case .dark = beat.state {
            XCTFail("with nothing to hear, silence can't be an alarm")
        }
    }

    // MARK: - the fleet's chatter doesn't shake every surface

    func testFleetBeatsCoalesce() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.recordBeat(source: .fleetCheckIn, now: t0)
        beat.recordBeat(source: .fleetCheckIn, now: t0.addingTimeInterval(20))
        XCTAssertEqual(beat.lastBeat, t0, "20-second refreshes must not republish every surface")

        beat.recordBeat(source: .fleetCheckIn, now: t0.addingTimeInterval(120))
        XCTAssertEqual(beat.lastBeat, t0.addingTimeInterval(120))
    }

    func testAVerificationIsNeverCoalescedAway() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        beat.recordBeat(source: .fleetCheckIn, now: t0)
        beat.recordBeat(source: .pathVerified, now: t0.addingTimeInterval(5))
        XCTAssertEqual(beat.lastVerified, t0.addingTimeInterval(5),
                       "a real test alert is rare, deliberate, and always worth stamping")
    }

    func testTheWireRoundsBeatsDownNeverUp() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }

        let beat = Heartbeat(defaults: defaults)
        let odd = Date(timeIntervalSince1970: 1_800_000_290)
        beat.recordBeat(source: .fleetCheckIn, now: odd)
        let wire = try XCTUnwrap(beat.wireLastBeat)
        XCTAssertLessThanOrEqual(wire, odd, "a liveness claim may only ever round to LOOKING OLDER")
        XCTAssertEqual(wire.timeIntervalSince1970.truncatingRemainder(dividingBy: 300), 0)
    }

    // MARK: - what counts as the fleet checking in

    func testDemoRowsNeverFeedTheHeartbeat() {
        var demo = Witness(id: "\(DemoFleet.idPrefix)porch")
        demo.link = .online
        XCTAssertFalse(FleetBeat.heard(in: [demo], demoPrefix: DemoFleet.idPrefix),
                       "sample data must never feed a liveness claim")

        var real = Witness(id: "canary-a3f7")
        real.link = .online
        XCTAssertTrue(FleetBeat.heard(in: [demo, real], demoPrefix: DemoFleet.idPrefix))
    }

    func testASilentDeviceIsNotACheckIn() {
        var lost = Witness(id: "canary-a3f7")
        lost.link = .lost
        XCTAssertFalse(FleetBeat.heard(in: [lost], demoPrefix: DemoFleet.idPrefix))
    }

    // MARK: - readable ages

    func testAgoStaysReadableAtEveryScale() {
        XCTAssertEqual(HeartbeatCopy.ago(30), "just now")
        XCTAssertEqual(HeartbeatCopy.ago(600), "10 min ago")
        XCTAssertEqual(HeartbeatCopy.ago(3600), "an hour ago")
        XCTAssertEqual(HeartbeatCopy.ago(7200), "2 hours ago")
        XCTAssertEqual(HeartbeatCopy.ago(86_400), "yesterday")
        XCTAssertEqual(HeartbeatCopy.ago(259_200), "3 days ago",
                       "a persisted verification can be days old; ‘4320 min ago’ is not a sentence")
    }
}
