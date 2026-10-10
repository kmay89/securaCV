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

    // MARK: - additive growth: what the device is, chirp, room, hub

    /// One row exactly as a phone from before these fields would send it.
    private let olderPhoneRow = #"""
    {"id":"canary-a3f7","name":"Porch","severityRaw":0,"linkRaw":1,"badgeRaw":3,
     "tamper":false,"isMuted":false,"batteryPct":80}
    """#

    func testAnOlderPhonesRowStillDecodesAndClaimsNothingNew() throws {
        let row = try WristSync.makeDecoder().decode(WristWitness.self,
                                                     from: Data(olderPhoneRow.utf8))
        XCTAssertNil(row.canIdentify, "no answer is no Chirp button, never a failing one")
        XCTAssertNil(row.publishedType)
        XCTAssertNil(row.hardware)
        XCTAssertEqual(row.deviceType, .unknown, "the generic marker, never a guess")
        XCTAssertEqual(row.hub, .unknown, "a hub nobody reported draws nothing")
        XCTAssertNil(DeviceGlanceCopy.hubLine(row.hub))
        XCTAssertNil(DeviceGlanceCopy.wellbeingLine(present: row.radarPresent,
                                                    occupants: row.radarOccupants,
                                                    breathing: row.breathingLock),
                     "absence is 'cannot say', never an empty calm room")
    }

    func testANewRowRoundTripsWhatItIsAndWhetherItCanChirp() throws {
        var row = WristSnapshot.sample(now: now).witnesses[0]
        row.canIdentify = true
        row.publishedType = "canary-wap"
        row.hubRaw = HubState.absent.rawValue
        row.radarPresent = true
        row.radarOccupants = 3
        row.breathingLock = false
        let data = try WristSync.makeEncoder().encode(row)
        let back = try WristSync.makeDecoder().decode(WristWitness.self, from: data)
        XCTAssertEqual(back, row)
        XCTAssertEqual(back.canIdentify, true)
        XCTAssertEqual(back.deviceType, .wap)
        XCTAssertEqual(back.hub, .absent)
        XCTAssertEqual(DeviceGlanceCopy.hubLine(back.hub), "No hub yet — it works on its own")
        XCTAssertEqual(DeviceGlanceCopy.wellbeingLine(present: back.radarPresent,
                                                      occupants: back.radarOccupants,
                                                      breathing: back.breathingLock),
                       "Someone present · 2+ in the room · breathing rhythm not sensed")
    }

    func testAnOlderPhonesMoodNumberIsIgnored() throws {
        // `anxiety` once rode the wire and nothing on the wrist read it; a
        // phone that still sends it must not break this watch.
        let data = try WristSync.makeEncoder().encode(WristSnapshot.sample(now: now))
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        json["anxiety"] = 9
        let decoded = try WristSync.makeDecoder().decode(
            WristSnapshot.self, from: JSONSerialization.data(withJSONObject: json))
        XCTAssertEqual(decoded, WristSnapshot.sample(now: now))
    }

    func testTheAckVerbIsPinned() {
        // Both ends compile this file, so the strings can't drift between
        // them — but an older build on the OTHER side keeps the old spelling
        // forever, so the spelling itself is the contract.
        XCTAssertEqual(WristSync.commandAck, "ack")
        XCTAssertEqual(WristSync.ackIDKey, "id")
        XCTAssertEqual(WristSync.messageCommandKey, "cmd")
    }

    @MainActor
    func testThePhoneAndTheWristNameTheWitnessCategoryOnce() {
        // The watch's custom long-look is registered for this id; the phone
        // stamps it on every witness alert. A respelling on either side
        // would silently drop the wrist back to the generic layout.
        XCTAssertEqual(AlertCenter.witnessCategoryID, NotificationIDs.witnessCategory)
        XCTAssertEqual(NotificationIDs.witnessCategory, "SECURACV_WITNESS")
    }

    // MARK: - the dead-man's-switch outranks green rows, on every glance

    func testAQuietFleetOverAFailedPathRaisesThePathAlarm() {
        var snap = WristSnapshot.sample(now: now)
        snap.severityRaw = Severity.ok.rawValue
        for state in [WristHeartbeatState.failed, .dark] {
            snap.heartbeatRaw = state.rawValue
            XCTAssertTrue(snap.pathAlarm, "\(state)")
            XCTAssertEqual(snap.glanceSymbol, state.sfSymbol)
            XCTAssertEqual(snap.glanceRole, state.role)
        }
    }

    func testAQuietFleetOverAWorkingPathIsJustQuiet() {
        var snap = WristSnapshot.sample(now: now)
        snap.severityRaw = Severity.ok.rawValue
        for state in [WristHeartbeatState.alive, .testing, .unknown] {
            snap.heartbeatRaw = state.rawValue
            XCTAssertFalse(snap.pathAlarm, "\(state)")
            XCTAssertEqual(snap.glanceSymbol, Severity.ok.sfSymbol)
        }
    }

    func testARealAlarmKeepsItsOwnGlyphOverADarkPath() {
        var snap = WristSnapshot.sample(now: now)
        snap.severityRaw = Severity.alert.rawValue
        snap.heartbeatRaw = WristHeartbeatState.dark.rawValue
        XCTAssertFalse(snap.pathAlarm)
        XCTAssertEqual(snap.glanceSymbol, Severity.alert.sfSymbol)
        XCTAssertEqual(snap.glanceRole, Severity.alert.role)
    }

    func testARoomWordAloneDoesNotRedrawTheComplications() {
        let before = WristSnapshot.sample(now: now)
        var after = before
        after.witnesses[0].radarPresent = true
        after.witnesses[1].isMuted = true
        after.sentAt = now
        XCTAssertTrue(after.drawsSameGlance(as: before),
                      "nothing a complication draws moved — no reload budget spent")
        after.severityRaw = Severity.alert.rawValue
        XCTAssertFalse(after.drawsSameGlance(as: before))
        var pathDown = before
        pathDown.severityRaw = Severity.ok.rawValue
        var later = pathDown
        pathDown.heartbeatRaw = WristHeartbeatState.dark.rawValue
        later.heartbeatRaw = WristHeartbeatState.dark.rawValue
        later.lastVerifiedAt = now.addingTimeInterval(-7_200)
        XCTAssertFalse(later.drawsSameGlance(as: pathDown),
                       "while the path is down its age IS the glance")
    }

    // MARK: - heartbeat wording (one sentence, both surfaces)

    func testHeartbeatSummaryMatchesThePhoneWordingExactly() {
        XCTAssertEqual(HeartbeatCopy.summary(state: .unknown, secondsSinceVerified: nil),
                       "Alert delivery not tested yet")
        XCTAssertEqual(HeartbeatCopy.summary(state: .alive, secondsSinceVerified: 30),
                       "Alert delivery confirmed just now")
        XCTAssertEqual(HeartbeatCopy.summary(state: .alive, secondsSinceVerified: 600),
                       "Alert delivery confirmed 10 min ago")
        XCTAssertEqual(HeartbeatCopy.summary(state: .testing, secondsSinceVerified: nil),
                       "Testing the whole path…")
        XCTAssertEqual(HeartbeatCopy.summary(state: .dark, secondsSinceVerified: 1_800),
                       "No heartbeat for 30 min — check your fleet")
        XCTAssertEqual(HeartbeatCopy.summary(state: .failed, secondsSinceVerified: nil,
                                             failureReason: "relay unreachable"),
                       "Test failed: relay unreachable")
    }

    func testDeliveryWordingNeverBorrowsVerified() {
        // "Verified" is reserved for an Ed25519 signature checked against a
        // pinned key (AGENTS.md rule 4). A notification iOS accepted is a
        // confirmed delivery, and every heartbeat sentence says so.
        for state in [WristHeartbeatState.unknown, .alive, .testing, .dark, .failed] {
            for source in [WristBeatSource?.none, .pathVerified, .fleetCheckIn] {
                for ago in [Int?.none, 30, 7_200] {
                    let line = HeartbeatCopy.summary(state: state, secondsSinceVerified: ago,
                                                     source: source)
                    XCTAssertFalse(line.lowercased().contains("verified"), line)
                }
            }
        }
    }

    func testSnapshotRendersItsOwnAgoFromTheAbsoluteDate() {
        var snapshot = WristSnapshot.sample(now: now)
        snapshot.heartbeatRaw = WristHeartbeatState.alive.rawValue
        snapshot.lastVerifiedAt = now.addingTimeInterval(-600)
        XCTAssertEqual(snapshot.heartbeatSummary(now: now), "Alert delivery confirmed 10 min ago")
        // A skewed watch clock must clamp, never show a negative age.
        snapshot.lastVerifiedAt = now.addingTimeInterval(120)
        XCTAssertEqual(snapshot.heartbeatSummary(now: now), "Alert delivery confirmed just now")
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
