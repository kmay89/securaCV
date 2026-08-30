//  ShelfCacheTests.swift — the shelf may only say what the Wall just said.
//
//  The Top Shelf provider is a separate process rendering from a cache, which
//  is exactly how a surface starts lying: the cache outlives the truth. These
//  pin the two defenses — the snapshot ages out instead of being trusted
//  forever, and its wording IS the fleet's own summary, never a re-telling.

import XCTest
@testable import WitnessWall

final class ShelfCacheTests: XCTestCase {

    /// A UserDefaults nobody else shares — the same isolation trick
    /// WallModelTests uses — so tests can't leak into each other or into a
    /// real device's app-group container.
    private func scratchDefaults() -> UserDefaults {
        UserDefaults(suiteName: UUID().uuidString)!
    }

    private func snapshot(asOf: Date = Date()) -> ShelfSnapshot {
        ShelfSnapshot(summary: "3 Canaries, all online", onlineCount: 3,
                      total: 3, hasChainTrouble: false, asOf: asOf)
    }

    // MARK: The cache itself — save, load, and every way to have nothing.

    func testSaveThenLoadRoundTrips() {
        let defaults = scratchDefaults()
        // A whole-second date, so Codable equality can't hinge on a Double's
        // fractional representation surviving JSON.
        let saved = snapshot(asOf: Date(timeIntervalSince1970: 1_700_000_000))

        ShelfCache.save(saved, to: defaults)

        XCTAssertEqual(ShelfCache.load(from: defaults), saved)
    }

    func testAnEmptySuiteLoadsNothing() {
        XCTAssertNil(ShelfCache.load(from: scratchDefaults()),
                     "no cache means no shelf — an absent answer, never an invented one")
    }

    func testGarbageInTheCacheLoadsNothing() {
        // tvOS purges aggressively and half-written blobs happen; unreadable
        // must degrade to the same honest place as absent.
        let defaults = scratchDefaults()
        defaults.set(Data("not a snapshot".utf8), forKey: ShelfCache.snapshotKey)

        XCTAssertNil(ShelfCache.load(from: defaults))
    }

    // MARK: Freshness — the cache ages out instead of being trusted forever.

    func testAFreshSnapshotIsCurrent() {
        let now = Date()
        XCTAssertTrue(snapshot(asOf: now).isCurrent(at: now))
        XCTAssertTrue(snapshot(asOf: now.addingTimeInterval(-60)).isCurrent(at: now))
    }

    func testAnOldSnapshotIsNotCurrent() {
        // Whole-second dates, so the boundary comparison is exact arithmetic
        // rather than whatever a fractional Date() rounds to.
        let now = Date(timeIntervalSince1970: 2_000_000_000)
        let atTheBoundary = snapshot(asOf: Date(timeIntervalSince1970: 2_000_000_000 - ShelfSnapshot.maxAge))
        XCTAssertFalse(atTheBoundary.isCurrent(at: now),
                       "at maxAge exactly the shelf goes quiet — a stale 'all online' "
                       + "is the lie this window exists to prevent")
        XCTAssertFalse(snapshot(asOf: now.addingTimeInterval(-3600)).isCurrent(at: now))
    }

    func testClockSkewIsNotStaleness() {
        let now = Date()
        XCTAssertTrue(snapshot(asOf: now.addingTimeInterval(120)).isCurrent(at: now),
                      "an asOf slightly in the future is skew across two processes, not age")
    }

    // MARK: Wording — one vocabulary on every surface.

    func testTheShelfCarriesTheFleetsOwnSummaryVerbatim() throws {
        let fleet = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"Porch","online":true},{"name":"Studio","online":false}]}"#.utf8))

        let shelf = ShelfSnapshot(fleet: fleet, asOf: Date())

        XCTAssertEqual(shelf.summary, fleet.summary,
                       "the shelf never re-words the fleet — FleetSnapshot.summary is "
                       + "the one sentence, verbatim")
        XCTAssertEqual(shelf.summary, "1 of 2 Canaries online")
        XCTAssertEqual(shelf.onlineCount, 1)
        XCTAssertEqual(shelf.total, 2)
        XCTAssertFalse(shelf.hasChainTrouble)
    }

    func testChainTroubleIsAppendedNeverHidden() throws {
        let fleet = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"Porch","online":true,"chain":"broken"}]}"#.utf8))

        let shelf = ShelfSnapshot(fleet: fleet, asOf: Date())

        XCTAssertTrue(shelf.hasChainTrouble)
        XCTAssertEqual(shelf.shelfTitle, "1 Canary, all online · a record needs attention",
                       "a troubled chain must reach the shelf line, not hide behind a calm count")

        let calm = snapshot()
        XCTAssertEqual(calm.shelfTitle, calm.summary,
                       "and a calm fleet gets the summary alone — no ornament")
    }

    func testAnExplicitlyUnknownChainIsNotTrouble() throws {
        // Displays honestly answer "unknown" — the wall learned not to paint
        // them orange (FleetSnapshot.Device.chainIsTroubled), and the shelf
        // inherits that line by construction.
        let fleet = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"Dash","online":true,"chain":"unknown"}]}"#.utf8))

        let shelf = ShelfSnapshot(fleet: fleet, asOf: Date())

        XCTAssertFalse(shelf.hasChainTrouble)
        XCTAssertEqual(shelf.shelfTitle, shelf.summary)
    }
}
