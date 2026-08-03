// WristSnapshotTests.swift
//
// The phone→watch contract, tested the way the wire will stress it: exact
// round-trips, out-of-order and duplicate deliveries, payloads from a newer
// phone, garbage, and the one-wording heartbeat summary both surfaces show.
// Everything here is pure Foundation with injected clocks — no WCSession,
// no mocks (the transport is Apple's; the CONTRACT is ours to prove).

import XCTest
@testable import SecuraCV

final class WristSnapshotTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_784_000_000)

    // MARK: - envelope

    func testEnvelopeRoundTripsExactly() throws {
        let snapshot = WristSnapshot.sample(now: now)
        let context = try WristSync.context(for: snapshot)
        XCTAssertEqual(WristSync.snapshot(fromContext: context), snapshot)
    }

    func testContextCarriesSchemaVersionBesideThePayload() throws {
        let context = try WristSync.context(for: .sample(now: now))
        XCTAssertEqual(WristSync.contextVersion(of: context), WristSnapshot.schemaVersion)
    }

    func testEncodingIsDeterministicSoContentDedupCanCompareBytes() throws {
        let snapshot = WristSnapshot.sample(now: now)
        let first = try WristSync.makeEncoder().encode(snapshot)
        let second = try WristSync.makeEncoder().encode(snapshot)
        XCTAssertEqual(first, second)
    }

    func testDecoderIgnoresUnknownFieldsFromANewerPhone() throws {
        // A future phone adds a field; this build must keep decoding.
        let data = try WristSync.makeEncoder().encode(WristSnapshot.sample(now: now))
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        json["someFutureField"] = "the watch has never heard of this"
        let grown = try JSONSerialization.data(withJSONObject: json)
        let decoded = WristSync.snapshot(fromContext: [
            WristSync.contextVersionKey: WristSnapshot.schemaVersion,
            WristSync.contextPayloadKey: grown,
        ])
        XCTAssertEqual(decoded, WristSnapshot.sample(now: now))
    }

    func testGarbagePayloadReturnsNilRatherThanThrowing() {
        let context: [String: Any] = [
            WristSync.contextVersionKey: WristSnapshot.schemaVersion,
            WristSync.contextPayloadKey: Data("not json".utf8),
        ]
        XCTAssertNil(WristSync.snapshot(fromContext: context))
    }

    func testAFutureSchemaIsDetectableWithoutDecoding() {
        let context: [String: Any] = [
            WristSync.contextVersionKey: WristSnapshot.schemaVersion + 1,
            WristSync.contextPayloadKey: Data(),
        ]
        XCTAssertNil(WristSync.snapshot(fromContext: context))
        let version = WristSync.contextVersion(of: context)
        XCTAssertNotNil(version)
        XCTAssertGreaterThan(version ?? 0, WristSnapshot.schemaVersion)
    }

    func testAStructurallyDecodableFutureSchemaIsStillRefused() throws {
        // A schema bump is reserved for changes an old reader would MISread —
        // so a future payload must be refused even when it happens to decode,
        // never rendered with old semantics.
        var fromTheFuture = WristSnapshot.sample(now: now)
        fromTheFuture.schema = WristSnapshot.schemaVersion + 1
        let context = try WristSync.context(for: fromTheFuture)
        XCTAssertNil(WristSync.snapshot(fromContext: context))
        XCTAssertEqual(WristSync.contextVersion(of: context), WristSnapshot.schemaVersion + 1)
    }

    // MARK: - adoption ordering

    func testHigherRevisionIsNews() {
        var older = WristSnapshot.sample(now: now)
        older.revision = 5
        var newer = older
        newer.revision = 6
        XCTAssertTrue(newer.isNewer(than: older))
        XCTAssertFalse(older.isNewer(than: newer))
    }

    func testLaterSentAtWinsWhenAReinstalledPhoneRestartsItsCounter() {
        var beforeReinstall = WristSnapshot.sample(now: now)
        beforeReinstall.revision = 500
        beforeReinstall.sentAt = now
        var afterReinstall = WristSnapshot.sample(now: now)
        afterReinstall.revision = 1
        afterReinstall.sentAt = now.addingTimeInterval(60)
        XCTAssertTrue(afterReinstall.isNewer(than: beforeReinstall))
    }

    func testADuplicateDeliveryIsNotNews() {
        let snapshot = WristSnapshot.sample(now: now)
        XCTAssertFalse(snapshot.isNewer(than: snapshot))
    }

    func testAnythingIsNewerThanNothing() {
        XCTAssertTrue(WristSnapshot.sample(now: now).isNewer(than: nil))
    }

    // MARK: - tolerant decoding of raw ladders

    func testUnknownRawBytesDegradeTolerantlyNeverFatally() {
        var snapshot = WristSnapshot.sample(now: now)
        snapshot.severityRaw = 200          // a ladder rung from the future
        snapshot.heartbeatRaw = 200
        var row = snapshot.witnesses[0]
        row.linkRaw = 200
        row.badgeRaw = 200
        XCTAssertEqual(snapshot.severity, .tamper)      // severity clamps UP — never understate
        XCTAssertEqual(snapshot.heartbeat, .unknown)
        XCTAssertEqual(row.link, .unknown)
        XCTAssertEqual(row.badge, .unknown)
    }

    // MARK: - heartbeat wording (one sentence, both surfaces)

    func testHeartbeatSummaryMatchesThePhoneWordingExactly() {
        XCTAssertEqual(HeartbeatCopy.summary(state: .unknown, secondsSinceVerified: nil),
                       "Not yet verified")
        XCTAssertEqual(HeartbeatCopy.summary(state: .alive, secondsSinceVerified: 30),
                       "Delivery verified just now")
        XCTAssertEqual(HeartbeatCopy.summary(state: .alive, secondsSinceVerified: 600),
                       "Delivery verified 10 min ago")
        XCTAssertEqual(HeartbeatCopy.summary(state: .testing, secondsSinceVerified: nil),
                       "Testing the whole path…")
        XCTAssertEqual(HeartbeatCopy.summary(state: .dark, secondsSinceVerified: 1_800),
                       "No heartbeat for 30 min — check your fleet")
        XCTAssertEqual(HeartbeatCopy.summary(state: .failed, secondsSinceVerified: nil,
                                             failureReason: "relay unreachable"),
                       "Test failed: relay unreachable")
    }

    func testSnapshotRendersItsOwnAgoFromTheAbsoluteDate() {
        var snapshot = WristSnapshot.sample(now: now)
        snapshot.heartbeatRaw = WristHeartbeatState.alive.rawValue
        snapshot.lastVerifiedAt = now.addingTimeInterval(-600)
        XCTAssertEqual(snapshot.heartbeatSummary(now: now), "Delivery verified 10 min ago")
        // A skewed watch clock must clamp, never show a negative age.
        snapshot.lastVerifiedAt = now.addingTimeInterval(120)
        XCTAssertEqual(snapshot.heartbeatSummary(now: now), "Delivery verified just now")
    }

    // MARK: - the sample's own honesty

    func testTheSampleIsDeterministicAndAlwaysFlaggedAsDemo() {
        XCTAssertEqual(WristSnapshot.sample(now: now), WristSnapshot.sample(now: now))
        XCTAssertTrue(WristSnapshot.sample(now: now).isDemoData)
        // And it never fakes an alarm (the DemoFleet rule).
        XCTAssertLessThanOrEqual(WristSnapshot.sample(now: now).severity, .notice)
    }

    // MARK: - the watch-local cache

    func testCacheRoundTripsThroughInjectedDefaults() throws {
        let suite = "test-wrist-cache-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        XCTAssertNil(WristCache.load(from: defaults))
        let snapshot = WristSnapshot.sample(now: now)
        WristCache.save(snapshot, to: defaults)
        XCTAssertEqual(WristCache.load(from: defaults), snapshot)
    }

    func testThePhoneGlanceCacheSpeaksTheSameContract() throws {
        let suite = "test-phone-glance-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        XCTAssertNil(PhoneGlanceCache.load(from: defaults))
        let snapshot = WristSnapshot.sample(now: now)
        PhoneGlanceCache.save(snapshot, to: defaults)
        XCTAssertEqual(PhoneGlanceCache.load(from: defaults), snapshot)
        // Two caches, two groups — never the same container (app groups
        // don't sync iPhone↔Watch; sharing a name would only lie about it).
        XCTAssertNotEqual(PhoneGlanceCache.appGroupID, WristCache.appGroupID)
    }
}
