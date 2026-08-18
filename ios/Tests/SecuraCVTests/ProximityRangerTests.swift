// ProximityRangerTests.swift
//
// The finding arithmetic, held still: smoothing eats jitter, bands are easy
// to enter and take a real drop to leave (no flicker at a boundary), a quiet
// beacon is "listening" rather than a stale "right here", the trend is the
// navigation aid, the haptic grammar ticks only on honest transitions, and
// the multi-Canary hint needs a clear margin before it renames the winner.

import XCTest
@testable import SecuraCV

final class ProximityRangerTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    /// Feed a steady level for a while so the EMA converges there.
    private func settle(_ ranger: inout ProximityRanger, at dbm: Int,
                        from start: Date, samples: Int = 30) -> ProximityRanger.Reading {
        var reading = ProximityRanger.Reading(band: .searching, trend: .unknown, smoothedDBM: nil)
        for i in 0..<samples {
            reading = ranger.ingest(rssiDBM: dbm,
                                    at: start.addingTimeInterval(Double(i) * 0.25))
        }
        return reading
    }

    func testFirstSampleSeedsTheAverage() {
        var ranger = ProximityRanger()
        let reading = ranger.ingest(rssiDBM: -70, at: t0)
        XCTAssertEqual(reading.smoothedDBM, -70)
    }

    func testAStrongSignalWalksUpTheBands() {
        var ranger = ProximityRanger()
        XCTAssertEqual(settle(&ranger, at: -80, from: t0).band, .far)
        XCTAssertEqual(settle(&ranger, at: -65, from: t0.addingTimeInterval(10)).band, .near)
        XCTAssertEqual(settle(&ranger, at: -55, from: t0.addingTimeInterval(20)).band, .veryClose)
        XCTAssertEqual(settle(&ranger, at: -45, from: t0.addingTimeInterval(30)).band, .here)
    }

    func testASmallDipDoesNotDemote() {
        // Converged just inside "near" (-73 entry); a wobble to a couple of
        // dB below the boundary must not flicker the label.
        XCTAssertEqual(ProximityRanger.nextBand(current: .near, smoothed: -75), .near)
        XCTAssertEqual(ProximityRanger.nextBand(current: .near, smoothed: -77.9), .near)
    }

    func testARealDropDemotes() {
        XCTAssertEqual(ProximityRanger.nextBand(current: .near, smoothed: -79), .far)
        XCTAssertEqual(ProximityRanger.nextBand(current: .here, smoothed: -70), .near)
    }

    func testPromotionUsesThePlainBoundary() {
        XCTAssertEqual(ProximityRanger.nextBand(current: .far, smoothed: -72), .near)
        XCTAssertEqual(ProximityRanger.nextBand(current: .searching, smoothed: -48), .here)
    }

    func testAQuietBeaconIsListeningNotStale() {
        var ranger = ProximityRanger()
        _ = settle(&ranger, at: -50, from: t0)
        // Ten silent seconds later the old "very close" must not survive.
        let reading = ranger.reading(at: t0.addingTimeInterval(30 * 0.25 + 10))
        XCTAssertEqual(reading.band, .searching)
        XCTAssertNil(reading.smoothedDBM)
    }

    func testWalkingTowardReadsWarmer() {
        var ranger = ProximityRanger()
        var reading = ranger.ingest(rssiDBM: -80, at: t0)
        for i in 1...10 {
            reading = ranger.ingest(rssiDBM: -80 + i * 2,
                                    at: t0.addingTimeInterval(Double(i) * 0.4))
        }
        XCTAssertEqual(reading.trend, .warmer)
    }

    func testWalkingAwayReadsColder() {
        var ranger = ProximityRanger()
        var reading = ranger.ingest(rssiDBM: -55, at: t0)
        for i in 1...10 {
            reading = ranger.ingest(rssiDBM: -55 - i * 2,
                                    at: t0.addingTimeInterval(Double(i) * 0.4))
        }
        XCTAssertEqual(reading.trend, .colder)
    }

    func testAMomentOfHistoryIsNeededBeforeATrend() {
        var ranger = ProximityRanger()
        let reading = ranger.ingest(rssiDBM: -60, at: t0)
        XCTAssertEqual(reading.trend, .unknown)
    }

    // MARK: - the haptic grammar

    func testTicksFireOnlyOnTransitions() {
        XCTAssertNil(ProximityRanger.tick(from: .near, to: .near))
    }

    func testCrossingInwardTicksGraded() {
        XCTAssertEqual(ProximityRanger.tick(from: .faint, to: .far), .closer(.far))
        XCTAssertEqual(ProximityRanger.tick(from: .far, to: .near), .closer(.near))
        XCTAssertEqual(ProximityRanger.tick(from: .near, to: .veryClose), .closer(.veryClose))
    }

    func testArrivalIsTheSuccessTap() {
        XCTAssertEqual(ProximityRanger.tick(from: .veryClose, to: .here), .arrived)
    }

    func testFallingBackIsSilent() {
        XCTAssertNil(ProximityRanger.tick(from: .here, to: .veryClose))
        XCTAssertNil(ProximityRanger.tick(from: .near, to: .far))
    }

    func testACloseSignalVanishingGetsOneNote() {
        XCTAssertEqual(ProximityRanger.tick(from: .veryClose, to: .searching), .lost)
        XCTAssertNil(ProximityRanger.tick(from: .faint, to: .searching),
                     "losing a faint signal is weather, not news")
    }

    func testFirstHearingAFaintBeaconStaysQuietInTheHand() {
        // The screen shows it; the hand is reserved for real progress.
        XCTAssertNil(ProximityRanger.tick(from: .searching, to: .faint))
    }

    // MARK: - the multi-Canary hint

    func testAClearlyStrongerNeighborIsNamed() {
        XCTAssertEqual(ProximityRanger.nearerNeighbor(
            targetDBM: -80, neighbors: [("Kitchen", -60), ("Garage", -85)]), "Kitchen")
    }

    func testANarrowLeadStaysQuiet() {
        XCTAssertNil(ProximityRanger.nearerNeighbor(
            targetDBM: -70, neighbors: [("Kitchen", -66)]),
            "ordinary noise must not rename the winner")
    }

    func testWithNoTargetSignalTheStrongestNeighborIsNamed() {
        XCTAssertEqual(ProximityRanger.nearerNeighbor(
            targetDBM: nil, neighbors: [("Kitchen", -75), ("Bedroom", -60)]), "Bedroom")
    }

    func testNoNeighborsNoHint() {
        XCTAssertNil(ProximityRanger.nearerNeighbor(targetDBM: -70, neighbors: []))
    }
}
