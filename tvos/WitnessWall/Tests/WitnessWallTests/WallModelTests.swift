//  WallModelTests.swift — the honesty rules, proved without a network.
//
//  The rule these all serve (docs/tvos/AUTOPIPELINE.md): drift is shown loudly
//  and calmly, NEVER silently rendered as fine. A wall that keeps drawing a
//  green fleet after the hub vanished is the exact failure this app exists to
//  not have, so it gets tests rather than good intentions.

import XCTest
@testable import WitnessWall

/// A transport with no socket behind it.
private final class StubTransport: FleetTransport, @unchecked Sendable {
    /// Answers, consumed in order; the last one repeats.
    var answers: [Result<String, Error>]
    private(set) var calls = 0

    init(_ answers: [Result<String, Error>]) { self.answers = answers }

    func fetchFleet(from base: URL) async throws -> String {
        let answer = answers[min(calls, answers.count - 1)]
        calls += 1
        switch answer {
        case .success(let body): return body
        case .failure(let error): throw error
        }
    }
}

private let goodFleet = #"{"kernel":"kitchen-hub","devices":[{"name":"Front Door"},{"name":"Studio"}]}"#

@MainActor
final class WallModelTests: XCTestCase {

    /// A UserDefaults nobody else shares, so tests can't leak into each other
    /// or into a real device's saved hub.
    private func scratchDefaults(_ name: String = UUID().uuidString) -> UserDefaults {
        UserDefaults(suiteName: name)!
    }

    /// Discovery answers nothing unless a test says otherwise — the default
    /// closure would run a REAL four-second Bonjour browse in the simulator,
    /// which is neither deterministic nor a unit test.
    private func model(_ answers: [Result<String, Error>], defaults: UserDefaults? = nil,
                       discover: @escaping @Sendable (TimeInterval) async -> [String] = { _ in [] }) -> WallModel {
        WallModel(transport: StubTransport(answers), defaults: defaults ?? scratchDefaults(),
                  pollInterval: 0.01, discover: discover)
    }

    func testStartsByAskingForAHub() {
        let m = model([.success(goodFleet)])
        XCTAssertEqual(m.state, .needsHub)
    }

    // MARK: The zero-typing path — the Wall finds the fleet by itself.

    func testSearchFindsTheFleetAtTheFirstWellKnownAddressAndPersistsIt() async {
        let defaults = scratchDefaults()
        let m = model([.success(goodFleet)], defaults: defaults)

        let found = await m.searchOnce()

        XCTAssertTrue(found)
        guard case .live(let fleet, _) = m.state else {
            return XCTFail("expected .live after a successful search, got \(m.state)")
        }
        XCTAssertEqual(fleet.kernel, "kitchen-hub")
        // The found address is saved, so the NEXT boot skips the search.
        XCTAssertEqual(defaults.string(forKey: "SecuraCVHubAddress"),
                       WallModel.wellKnownCandidates.first)
    }

    func testSearchSkipsAnAddressThatAnswersWithSomethingThatIsNotAFleet() async {
        // canary.local can be squatted by a captive portal or someone else's
        // box. An answer that doesn't parse as a fleet must be SKIPPED — the
        // next candidate can still win — and never persisted.
        let defaults = scratchDefaults()
        let m = model([
            .success("<html>totally a login page</html>"),
            .success(goodFleet),
        ], defaults: defaults)

        let found = await m.searchOnce()

        XCTAssertTrue(found, "the second candidate should still be tried and win")
        XCTAssertEqual(defaults.string(forKey: "SecuraCVHubAddress"),
                       WallModel.wellKnownCandidates[1],
                       "the address persisted must be the one that actually answered with a fleet")
    }

    func testAFailedSearchFallsBackToAskingForAnAddress() async {
        let m = model([.failure(FleetError.unreachable("nothing there"))])

        let found = await m.searchOnce()

        XCTAssertFalse(found)
        XCTAssertEqual(m.state, .needsHub,
                       "when nothing answers, the Wall asks — it never pretends to be connected")
    }

    // MARK: The hubless path — nothing claims canary.local, so the Canaries
    // are found by their own announcements and merged into one wall.

    func testAHublessFleetIsFoundByItsAnnouncementsAndMerged() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let bedroom = #"{"devices":[{"name":"Bedroom 7\"","online":true}]}"#
        let defaults = scratchDefaults()
        // Both well-known candidates fail (no hub), then each announced
        // Canary answers for itself.
        let m = model([
            .failure(FleetError.unreachable("no hub")),
            .failure(FleetError.unreachable("no wap")),
            .success(porch),
            .success(bedroom),
        ], defaults: defaults, discover: { _ in
            ["canary-nightstand-001-aa.local", "canary-nightstand7-001-bb.local"]
        })

        let found = await m.searchOnce()

        XCTAssertTrue(found, "two standalone Canaries are a fleet, hub or not")
        guard case .live(let fleet, _) = m.state else {
            return XCTFail("expected .live after discovery, got \(m.state)")
        }
        XCTAssertEqual(fleet.devices.map(\.name).sorted(), ["Bedroom 7\"", "Porch"])
        XCTAssertNil(fleet.kernel, "no single device gets to name the merged fleet")
        XCTAssertEqual(defaults.stringArray(forKey: "SecuraCVWallSources"),
                       ["canary-nightstand-001-aa.local", "canary-nightstand7-001-bb.local"],
                       "every confirmed source persists, so the next boot polls them all")
        XCTAssertEqual(m.hubAddress, "2 Canaries · found on your network")
    }

    func testOneDarkCanaryDoesNotBlankTheOnesStillTalking() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        // First source answers, second is dark. calls: a → porch, b → failure.
        let m = model([
            .success(porch),
            .failure(FleetError.unreachable("asleep")),
        ], defaults: defaults)

        await m.refreshOnce()

        guard case .live(let fleet, _) = m.state else {
            return XCTFail("a partial answer is still a live wall, got \(m.state)")
        }
        XCTAssertEqual(fleet.devices.map(\.name), ["Porch"])
    }

    func testMergeDedupesByNameAndRefusesToInventAVerdict() throws {
        let a = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"kernel":"hub-a","verified_through":"now","devices":[{"name":"Porch"}]}"#.utf8))
        let b = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"kernel":"hub-b","verified_through":"now","devices":[{"name":"Porch"},{"name":"Studio"}]}"#.utf8))

        let merged = FleetSnapshot.merged([a, b])

        XCTAssertEqual(merged.devices.map(\.name), ["Porch", "Studio"],
                       "a device reported twice is one device — first answer wins")
        XCTAssertNil(merged.kernel, "two kernels means no one name won")
        XCTAssertNil(merged.verifiedThrough,
                     "the merge of two verification claims is not itself verified")

        let alone = FleetSnapshot.merged([a])
        XCTAssertEqual(alone.kernel, "hub-a", "a single part keeps its own name and verdict")
        XCTAssertEqual(alone.verifiedThrough, "now")
    }

    func testAGoodFetchGoesLive() async {
        let m = model([.success(goodFleet)])
        m.connect(to: "http://canary.local:8099")
        await m.refreshOnce()

        guard case .live(let fleet, _) = m.state else {
            return XCTFail("expected .live, got \(m.state)")
        }
        XCTAssertEqual(fleet.kernel, "kitchen-hub")
        XCTAssertEqual(fleet.devices.count, 2)
    }

    func testLosingTheHubMarksTheFleetStaleInsteadOfDrawingItAsCurrent() async {
        let m = model([
            .success(goodFleet),
            .failure(FleetError.unreachable("the network went away")),
        ])
        m.connect(to: "http://canary.local:8099")
        await m.refreshOnce()          // live
        await m.refreshOnce()          // hub gone

        guard case .stale(let fleet, _, let reason) = m.state else {
            return XCTFail("expected .stale, got \(m.state)")
        }
        // The last good fleet is still shown — but as stale, with a reason.
        XCTAssertEqual(fleet.devices.count, 2)
        XCTAssertFalse(reason.isEmpty)
    }

    func testStaleKeepsTheORIGINALTimestampAcrossRepeatedFailures() async {
        // Regression guard: if each failure refreshed `since`, the screen would
        // claim the record was verified moments ago forever.
        let m = model([
            .success(goodFleet),
            .failure(FleetError.unreachable("gone")),
            .failure(FleetError.unreachable("still gone")),
        ])
        m.connect(to: "http://canary.local:8099")
        await m.refreshOnce()
        await m.refreshOnce()
        guard case .stale(_, let firstSince, _) = m.state else {
            return XCTFail("expected .stale, got \(m.state)")
        }
        await m.refreshOnce()
        guard case .stale(_, let secondSince, _) = m.state else {
            return XCTFail("expected .stale, got \(m.state)")
        }
        XCTAssertEqual(firstSince, secondSince, "the 'verified at' time must not creep forward while offline")
    }

    func testNeverHavingConnectedSaysSoRatherThanShowingAnEmptyWall() async {
        let m = model([.failure(FleetError.unreachable("no route to host"))])
        m.connect(to: "http://canary.local:8099")
        await m.refreshOnce()

        guard case .unreachable(let reason) = m.state else {
            return XCTFail("expected .unreachable, got \(m.state)")
        }
        XCTAssertFalse(reason.isEmpty)
    }

    func testRecoveringGoesBackToLive() async {
        let m = model([
            .failure(FleetError.unreachable("hub rebooting")),
            .success(goodFleet),
        ])
        m.connect(to: "http://canary.local:8099")
        await m.refreshOnce()
        await m.refreshOnce()

        guard case .live = m.state else {
            return XCTFail("expected .live after recovery, got \(m.state)")
        }
    }

    func testATypoIsNotPersisted() {
        let defaults = scratchDefaults()
        let m = model([.success(goodFleet)], defaults: defaults)
        m.connect(to: "   ")

        guard case .unreachable = m.state else {
            return XCTFail("expected .unreachable for an unusable address, got \(m.state)")
        }
        XCTAssertNil(defaults.string(forKey: "SecuraCVHubAddress"),
                     "a bad address must not be saved, or it re-fails on every boot")
    }

    func testAGoodAddressIsPersistedSoAPowerCutHealsItself() {
        let defaults = scratchDefaults()
        let m = model([.success(goodFleet)], defaults: defaults)
        m.connect(to: "canary.local:8099")
        XCTAssertEqual(defaults.string(forKey: "SecuraCVHubAddress"), "canary.local:8099")

        // A fresh model (as after a reboot) picks the hub back up with no help.
        let rebooted = WallModel(transport: StubTransport([.success(goodFleet)]), defaults: defaults)
        XCTAssertEqual(rebooted.hubAddress, "canary.local:8099")
        _ = m
    }
}

// MARK: - Address handling

final class FleetAddressTests: XCTestCase {

    func testABareHostGetsHTTPBecauseThatIsTheLANCase() throws {
        let url = try FleetAddress.normalize("canary.local:8099")
        XCTAssertEqual(url.scheme, "http")
        XCTAssertEqual(url.host, "canary.local")
    }

    func testAnExplicitSchemeIsRespected() throws {
        let url = try FleetAddress.normalize("https://hub.example.com")
        XCTAssertEqual(url.scheme, "https")
    }

    func testEmptyIsRejected() {
        XCTAssertThrowsError(try FleetAddress.normalize("   "))
    }

    func testTheEndpointIsAppendedOnce() throws {
        let base = try FleetAddress.normalize("canary.local:8099")
        XCTAssertEqual(FleetAddress.endpoint(for: base).path, "/api/fleet")
    }

    func testPastingTheFullEndpointDoesNotDoubleIt() throws {
        let base = try FleetAddress.normalize("http://canary.local:8099/api/fleet")
        XCTAssertEqual(FleetAddress.endpoint(for: base).path, "/api/fleet")
    }
}

// MARK: - Backoff

final class BackoffTests: XCTestCase {

    func testItDoublesThenStopsAtTheCap() {
        var backoff = Backoff(base: 2, cap: 30)
        // Take the ceiling (not the jittered value) so this is deterministic.
        var ceilings: [TimeInterval] = []
        for _ in 0..<8 {
            ceilings.append(backoff.nextCeiling)
            _ = backoff.nextDelay { $0.upperBound }
        }
        XCTAssertEqual(Array(ceilings.prefix(5)), [2, 4, 8, 16, 30])
        XCTAssertTrue(ceilings.allSatisfy { $0 <= 30 }, "an unplugged hub must not hammer the LAN")
    }

    func testJitterStaysWithinTheCeiling() {
        var backoff = Backoff(base: 2, cap: 30)
        for _ in 0..<20 {
            let ceiling = backoff.nextCeiling
            let delay = backoff.nextDelay()
            XCTAssertGreaterThanOrEqual(delay, 0)
            XCTAssertLessThanOrEqual(delay, ceiling)
        }
    }

    func testASuccessfulFetchResetsTheLadder() {
        var backoff = Backoff(base: 2, cap: 60)
        for _ in 0..<5 { _ = backoff.nextDelay { $0.upperBound } }
        XCTAssertGreaterThan(backoff.nextCeiling, 2)
        backoff.reset()
        XCTAssertEqual(backoff.nextCeiling, 2, "one blip must not leave the Wall polling once a minute all evening")
    }
}
