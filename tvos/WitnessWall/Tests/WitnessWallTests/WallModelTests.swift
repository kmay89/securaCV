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

/// A transport that also serves a sealed log — the seam for proving the
/// verify wiring (WallModel.refreshVerification) without a socket. It
/// records the bearer each sealed-log request carried, and can play a hub
/// that gates the route: with `acceptedToken` set, any other token (or
/// none) is answered 401, exactly as the kernel answers.
private final class SealedLogTransport: FleetTransport, @unchecked Sendable {
    /// Fleet answers, consumed in order; the last one repeats.
    var fleetAnswers: [Result<String, Error>]
    let sealedLog: String?
    var acceptedToken: String?
    /// The token on every sealed-log request, in order (nil = none sent).
    private(set) var tokensSent: [String?] = []
    /// The base URL of every sealed-log request, in order.
    private(set) var sealedLogBases: [URL] = []
    private var calls = 0

    init(fleet: [Result<String, Error>], sealedLog: String?, acceptedToken: String? = nil) {
        self.fleetAnswers = fleet
        self.sealedLog = sealedLog
        self.acceptedToken = acceptedToken
    }

    func fetchFleet(from base: URL) async throws -> String {
        let answer = fleetAnswers[min(calls, fleetAnswers.count - 1)]
        calls += 1
        switch answer {
        case .success(let body): return body
        case .failure(let error): throw error
        }
    }

    func fetchSealedLog(from base: URL, token: String?) async -> SealedLogFetch {
        tokensSent.append(token)
        sealedLogBases.append(base)
        if let acceptedToken, token != acceptedToken { return .unauthorized }
        guard let sealedLog else { return .absent }
        return .document(sealedLog)
    }
}

/// Pairings kept in memory — the model's pairing decisions, provable on a
/// simulator host the Keychain may refuse (WallPairingTests covers the
/// Keychain itself).
final class MemoryPairingSecrets: PairingSecrets, @unchecked Sendable {
    private var items: [String: Data] = [:]
    func read(account: String) -> Data? { items[account] }
    func write(_ data: Data, account: String) throws { items[account] = data }
    func remove(account: String) { items[account] = nil }
    var accounts: [String] { Array(items.keys).sorted() }
}

private let goodFleet = #"{"kernel":"kitchen-hub","devices":[{"name":"Front Door"},{"name":"Studio"}]}"#

/// A pairing receipt in `witness_api mint-viewer-token`'s shape.
func receiptJSON(token: String, key: String, baseURL: String? = nil) -> String {
    var fields = [
        #""kernel":"witness-kernel""#,
        #""sealed_log_token":"\#(token)""#,
        #""verifying_key":"\#(key)""#,
        #""token_id":"\#(token.prefix(8))""#,
    ]
    if let baseURL { fields.append(#""base_url":"\#(baseURL)""#) }
    return "{" + fields.joined(separator: ",") + "}"
}

/// The kernel's own sealed-log document
/// (tests/fixtures/envelope/sealed_log_document_vector.json): a log the core
/// verifies, under a key the kernel really signed with, whose payloads carry
/// the time buckets the timeline draws. Read from the checkout through
/// #filePath — the iPhone tests' idiom — and skipped, never failed, when the
/// checkout is not visible from the test host; witness-core's vectors test
/// stays the durable gate for the document itself.
func sharedSealedLogVector() throws -> String {
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()   // WitnessWallTests
        .deletingLastPathComponent()   // Tests
        .deletingLastPathComponent()   // WitnessWall
        .deletingLastPathComponent()   // tvos
        .deletingLastPathComponent()   // repo root
    let url = repoRoot.appendingPathComponent("tests/fixtures/envelope/sealed_log_document_vector.json")
    try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                      "repo checkout not visible from the test host; " +
                      "tvos/witness-core/tests/vectors.rs remains the gate for this document")
    return try String(contentsOf: url, encoding: .utf8)
}

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
        // Every well-known candidate fails (no hub, no WAP), then each
        // announced Canary answers for itself.
        let noHub: [Result<String, Error>] = WallModel.wellKnownCandidates.map { _ in
            .failure(FleetError.unreachable("no hub"))
        }
        let m = model(noHub + [
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

    func testADarkCanaryStaysOnTheWallAsOffline() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let bedroom = #"{"devices":[{"name":"Bedroom","online":true}]}"#
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        // Cycle 1: both answer. Cycle 2: b has gone dark.
        let m = model([
            .success(porch),
            .success(bedroom),
            .success(porch),
            .failure(FleetError.unreachable("asleep")),
        ], defaults: defaults)

        await m.refreshOnce()
        await m.refreshOnce()

        guard case .live(let fleet, _) = m.state else {
            return XCTFail("a partial answer is still a live wall, got \(m.state)")
        }
        XCTAssertEqual(fleet.devices.map(\.name), ["Porch", "Bedroom"],
                       "the dark source's devices stay on the wall — vanishing "
                       + "would let the merge read as all-online")
        XCTAssertEqual(fleet.devices.map(\.online), [true, false],
                       "and they are shown as what they are: unreachable")
        XCTAssertEqual(fleet.onlineCount, 1)
    }

    func testASourceThatNeverAnsweredIsNotInvented() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        // b has NEVER answered — there is nothing honest to remember.
        let m = model([
            .success(porch),
            .failure(FleetError.unreachable("never met")),
        ], defaults: defaults)

        await m.refreshOnce()

        guard case .live(let fleet, _) = m.state else {
            return XCTFail("expected .live, got \(m.state)")
        }
        XCTAssertEqual(fleet.devices.map(\.name), ["Porch"],
                       "no last answer means no remembered devices — absence, not invention")
    }

    func testMergeNeverCarriesANameOrAVerdictAcrossSources() throws {
        let a = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"kernel":"hub-a","verified_through":"now","devices":[{"name":"Porch"}]}"#.utf8))
        let b = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"Porch"},{"name":"Studio"}]}"#.utf8))

        let merged = FleetSnapshot.merged([a, b])

        XCTAssertEqual(merged.devices.map(\.name), ["Porch", "Studio"],
                       "an untagged duplicate is one device — first answer wins")
        XCTAssertNil(merged.kernel,
                     "one part's name over another part's devices is a claim nobody made")
        XCTAssertNil(merged.verifiedThrough,
                     "a verdict from ONE part must not banner the whole merged fleet")

        let alone = FleetSnapshot.merged([a])
        XCTAssertEqual(alone.kernel, "hub-a", "a single part keeps its own name and verdict")
        XCTAssertEqual(alone.verifiedThrough, "now")
    }

    func testTwoUnitsSharingAStaleDefaultNameStayTwoRows() throws {
        // Older firmware ships a compile-time default device_id, so a hubless
        // home can genuinely hold two "canary_dash_001"s. Their sources tell
        // them apart, and collapsing them would drop a real Canary.
        let a = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"canary_dash_001"}]}"#.utf8)).tagged(bySource: "a.local")
        let b = try JSONDecoder().decode(FleetSnapshot.self, from: Data(
            #"{"devices":[{"name":"canary_dash_001"}]}"#.utf8)).tagged(bySource: "b.local")

        let merged = FleetSnapshot.merged([a, b])

        XCTAssertEqual(merged.devices.count, 2, "same name, two sources — two physical units")
        XCTAssertEqual(Set(merged.devices.map(\.id)).count, 2,
                       "and their ids differ, or SwiftUI's ForEach folds them back into one")
    }

    func testDiscoveryKeepsListeningAndANewCanaryJoinsTheWall() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let attic = #"{"devices":[{"name":"Attic","online":true}]}"#
        let defaults = scratchDefaults()
        defaults.set(["a.local"], forKey: "SecuraCVWallSources")
        defaults.set(true, forKey: "SecuraCVWallSourcesDiscovered")
        // reconcile probes the NEW host (attic), then the follow-up refresh
        // polls both sources.
        let m = model([
            .success(attic),
            .success(porch),
            .success(attic),
        ], defaults: defaults, discover: { _ in ["a.local", "new.local"] })

        await m.reconcileDiscovered()

        XCTAssertEqual(defaults.stringArray(forKey: "SecuraCVWallSources"),
                       ["a.local", "new.local"],
                       "a Canary plugged in later joins without a settings reset")
        guard case .live(let fleet, _) = m.state else {
            return XCTFail("expected .live after reconcile, got \(m.state)")
        }
        XCTAssertEqual(fleet.devices.map(\.name).sorted(), ["Attic", "Porch"])
    }

    // MARK: Pruning — a discovered source is a claim, and claims expire.

    private static let thirtyOneDays: TimeInterval = 31 * 24 * 60 * 60

    func testADiscoveredSourceNotSeenForThirtyDaysIsDroppedOnLoad() {
        let defaults = scratchDefaults()
        defaults.set(["fresh.local", "gone.local"], forKey: "SecuraCVWallSources")
        defaults.set(true, forKey: "SecuraCVWallSourcesDiscovered")
        let now = Date().timeIntervalSince1970
        defaults.set(["fresh.local": now - 60, "gone.local": now - Self.thirtyOneDays],
                     forKey: "SecuraCVWallSourcesSeen")

        let m = model([.success(goodFleet)], defaults: defaults)

        XCTAssertEqual(m.sources, ["fresh.local"],
                       "a Canary that moved out — or a stranger's advert — is not polled forever")
        XCTAssertEqual(defaults.stringArray(forKey: "SecuraCVWallSources"), ["fresh.local"],
                       "and the pruned list is what the next boot reads")
        let seen = defaults.dictionary(forKey: "SecuraCVWallSourcesSeen") ?? [:]
        XCTAssertNil(seen["gone.local"], "the dropped source's record goes with it")
        XCTAssertNotNil(seen["fresh.local"])
    }

    func testASourceSavedBeforeTheTableExistedIsKeptAndStampedNow() {
        // Upgrading must not blank a working wall: with no record, the clock
        // starts today rather than at the epoch.
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        defaults.set(true, forKey: "SecuraCVWallSourcesDiscovered")

        let m = model([.success(goodFleet)], defaults: defaults)

        XCTAssertEqual(m.sources, ["a.local", "b.local"])
        let seen = defaults.dictionary(forKey: "SecuraCVWallSourcesSeen") ?? [:]
        XCTAssertEqual(Set(seen.keys), Set(["a.local", "b.local"]),
                       "every kept source now has a record to age against")
    }

    func testATypedHubIsNotPrunedByAge() {
        // One deliberate address, re-validated every poll; forgetting it after
        // a month away would send someone back to typing with a TV remote.
        let defaults = scratchDefaults()
        defaults.set(["hub.example.local:8099"], forKey: "SecuraCVWallSources")
        defaults.set(false, forKey: "SecuraCVWallSourcesDiscovered")
        defaults.set(["hub.example.local:8099": Date().timeIntervalSince1970 - Self.thirtyOneDays],
                     forKey: "SecuraCVWallSourcesSeen")

        let m = model([.success(goodFleet)], defaults: defaults)

        XCTAssertEqual(m.sources, ["hub.example.local:8099"])
    }

    func testAnAnsweringSourceRestartsItsThirtyDayClock() async {
        let defaults = scratchDefaults()
        defaults.set(["a.local"], forKey: "SecuraCVWallSources")
        defaults.set(true, forKey: "SecuraCVWallSourcesDiscovered")
        let old = Date().timeIntervalSince1970 - 20 * 24 * 60 * 60
        defaults.set(["a.local": old], forKey: "SecuraCVWallSourcesSeen")
        let m = model([.success(goodFleet)], defaults: defaults)

        await m.refreshOnce()

        let seen = defaults.dictionary(forKey: "SecuraCVWallSourcesSeen") ?? [:]
        let stamp = seen["a.local"] as? Double ?? 0
        XCTAssertGreaterThan(stamp, old, "every real answer restarts the clock")
    }

    func testADarkSourceDoesNotGetItsClockRestarted() async {
        let porch = #"{"devices":[{"name":"Porch","online":true}]}"#
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        defaults.set(true, forKey: "SecuraCVWallSourcesDiscovered")
        let old = Date().timeIntervalSince1970 - 20 * 24 * 60 * 60
        defaults.set(["a.local": old, "b.local": old], forKey: "SecuraCVWallSourcesSeen")
        let m = model([
            .success(porch),
            .failure(FleetError.unreachable("asleep")),
        ], defaults: defaults)

        await m.refreshOnce()

        let seen = defaults.dictionary(forKey: "SecuraCVWallSourcesSeen") ?? [:]
        XCTAssertGreaterThan(seen["a.local"] as? Double ?? 0, old)
        XCTAssertEqual(seen["b.local"] as? Double, old,
                       "silence is not an answer — only a real fleet restarts the clock")
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

    // MARK: This TV's own verification — what may (and may not) say "Verified".

    /// A document that IS a sealed log, under a real key, whose chain is
    /// broken: the first entry claims a prev_hash that is not the genesis
    /// head. The verifying key is the Ed25519 base point — a valid key — so
    /// the core reaches the chain walk and fails THERE, the way a tampered
    /// log fails, not the way garbage fails.
    private var tamperedSealedLog: String {
        """
        {
          "verifying_key": "5866666666666666666666666666666666666666666666666666666666666666",
          "entries": [
            {"id": 7, "payload": "{}",
             "prev_hash": "\(String(repeating: "a", count: 64))",
             "entry_hash": "\(String(repeating: "b", count: 64))",
             "signature": "\(String(repeating: "c", count: 128))"}
          ]
        }
        """
    }

    private func singleSourceDefaults() -> UserDefaults {
        let defaults = scratchDefaults()
        defaults.set(["canary.local:8099"], forKey: "SecuraCVWallSources")
        return defaults
    }

    func testNoSealedLogServedMeansNoVerdict() async {
        // Today this is EVERY source — no kernel or firmware ships the
        // endpoint yet — and the honest answer is no verdict at all: the
        // banner then says the fleet REPORTS verified, never that this TV
        // verified anything.
        let m = WallModel(transport: SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: nil),
                          defaults: singleSourceDefaults(), pollInterval: 0.01, discover: { _ in [] })
        await m.refreshOnce()

        guard case .live = m.state else {
            return XCTFail("expected .live, got \(m.state)")
        }
        XCTAssertNil(m.report)
    }

    func testAServedSealedLogBecomesThisTVsOwnVerdict() async throws {
        let m = WallModel(
            transport: SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog),
            defaults: singleSourceDefaults(), pollInterval: 0.01, discover: { _ in [] })
        await m.refreshOnce()

        let report = try XCTUnwrap(m.report, "a served sealed log must be verified, not ignored")
        XCTAssertFalse(report.ok, "a broken chain must never render as fine")
        XCTAssertEqual(report.failedAt, 7, "the verdict names the entry that broke")
    }

    func testAnAnswerThatIsNotASealedLogIsANonAnswerNotAFailedVerification() async {
        // A squatted host answering 200 HTML must not put an alarm on the
        // household's wall — the same "keep looking, never close enough"
        // rule the fleet parse applies to captive portals.
        let m = WallModel(
            transport: SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: "<html>Sign in</html>"),
            defaults: singleSourceDefaults(), pollInterval: 0.01, discover: { _ in [] })
        await m.refreshOnce()

        XCTAssertNil(m.report)
    }

    func testAMultiSourceWallCarriesNoVerdict() async {
        // One chain from one Canary must not banner devices another one
        // reported — the same rule merged() applies to verified_through.
        let defaults = scratchDefaults()
        defaults.set(["a.local", "b.local"], forKey: "SecuraCVWallSources")
        let m = WallModel(
            transport: SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog),
            defaults: defaults, pollInterval: 0.01, discover: { _ in [] })
        await m.refreshOnce()

        guard case .live = m.state else {
            return XCTFail("expected .live, got \(m.state)")
        }
        XCTAssertNil(m.report)
    }

    func testLosingTheSourceDropsTheVerdict() async {
        // A remembered verdict is not a current verdict — the same rule
        // withEveryDeviceOffline applies to a remembered verified_through.
        let m = WallModel(
            transport: SealedLogTransport(
                fleet: [.success(goodFleet), .failure(FleetError.unreachable("gone"))],
                sealedLog: tamperedSealedLog),
            defaults: singleSourceDefaults(), pollInterval: 0.01, discover: { _ in [] })
        await m.refreshOnce()
        XCTAssertNotNil(m.report, "the served sealed log earned a verdict while live")

        await m.refreshOnce()
        guard case .stale = m.state else {
            return XCTFail("expected .stale, got \(m.state)")
        }
        XCTAssertNil(m.report)
    }

    // MARK: Pairing — the viewer token goes where it was paired, and
    // "Verified" means the pinned key.

    private let viewerToken = String(repeating: "7", count: 64)
    private let otherKey = String(repeating: "1", count: 64)

    /// A single-source model over a sealed-log transport, with pairings in
    /// memory (the model's decisions, not the Keychain's, are under test).
    private func pairedModel(_ transport: SealedLogTransport,
                             secrets: MemoryPairingSecrets = MemoryPairingSecrets(),
                             sources: [String] = ["canary.local:8099"]) -> WallModel {
        let defaults = scratchDefaults()
        defaults.set(sources, forKey: "SecuraCVWallSources")
        return WallModel(transport: transport, defaults: defaults, pollInterval: 0.01,
                         pairings: PairedSourceStore(secrets: secrets), discover: { _ in [] })
    }

    func testAnUnpairedWallAsksWithoutATokenAndClaimsNothingPinned() async {
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog)
        let m = pairedModel(transport)
        await m.refreshOnce()

        XCTAssertEqual(transport.tokensSent, [nil], "no pairing, no bearer")
        XCTAssertNotNil(m.report)
        XCTAssertEqual(m.standing, .unpaired,
                       "a walk against the log's own key is never a pinned claim")
        XCTAssertNil(m.pairing)
    }

    func testAnUnpairedWallThatTheKernelRefusesHasNoVerdictAtAll() async {
        // Today's kernel: the route is gated and an unpaired TV holds no
        // token. That is the honest "no verdict" — not a refused pairing.
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog,
                                           acceptedToken: viewerToken)
        let m = pairedModel(transport)
        await m.refreshOnce()

        XCTAssertNil(m.report)
        XCTAssertEqual(m.standing, .none)
    }

    func testPairingSendsTheTokenToThatSourceAndNoOther() async throws {
        let secrets = MemoryPairingSecrets()
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog)
        let m = pairedModel(transport, secrets: secrets)
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        XCTAssertEqual(m.pairing?.verifyingKey, otherKey)
        XCTAssertEqual(secrets.accounts, ["http://canary.local:8099"])

        await m.refreshOnce()
        XCTAssertEqual(transport.tokensSent, [viewerToken])

        // Another source on the same TV — same Keychain, no pairing there —
        // is asked without the token.
        let elsewhere = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog)
        let other = pairedModel(elsewhere, secrets: secrets, sources: ["192.168.1.20:8799"])
        await other.refreshOnce()
        XCTAssertEqual(elsewhere.tokensSent, [nil], "a viewer token is never sent to a source it was not paired with")
        XCTAssertNil(other.pairing)
    }

    func testAPairedWallWhoseTokenIsRefusedSaysSoAndCarriesNoVerdict() async {
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog,
                                           acceptedToken: String(repeating: "9", count: 64))
        let m = pairedModel(transport)
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        await m.refreshOnce()

        XCTAssertEqual(transport.tokensSent, [viewerToken])
        XCTAssertNil(m.report, "a refused read is no walk at all")
        XCTAssertEqual(m.standing, .unauthorized, "a revoked pairing must never read as the calm self-report")
    }

    func testARefusingSourceIsAskedOnceNotEveryCycle() async {
        // The kernel counts a refused (or missing) credential toward a
        // per-address lockout that also closes /api/fleet: a Wall knocking
        // every ten seconds would lock itself out of its own roll-call.
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog,
                                           acceptedToken: String(repeating: "9", count: 64))
        let m = pairedModel(transport)
        await m.refreshOnce()
        await m.refreshOnce()
        await m.refreshOnce()
        XCTAssertEqual(transport.tokensSent.count, 1, "unpaired and refused: asked once")
        XCTAssertEqual(m.standing, .none)

        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        await m.refreshOnce()
        await m.refreshOnce()
        XCTAssertEqual(transport.tokensSent, [nil, viewerToken],
                       "a new pairing earns one new knock, and a refusal ends it again")
        XCTAssertEqual(m.standing, .unauthorized, "the refusal is still said, not forgotten")

        transport.acceptedToken = viewerToken
        m.forgetPairing()
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        await m.refreshOnce()
        XCTAssertEqual(transport.tokensSent.last, .some(viewerToken))
        XCTAssertEqual(m.standing, .keyChanged(pinned: otherKey,
                                               served: "5866666666666666666666666666666666666666666666666666666666666666"))
    }

    func testThePinnedKeySigningAPassingLogIsTheOneVerifiedStanding() async throws {
        let vector = try sharedSealedLogVector()
        let key = try XCTUnwrap(VerificationStanding.servedKey(in: vector))
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: vector,
                                           acceptedToken: viewerToken)
        let m = pairedModel(transport)
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: key)))
        await m.refreshOnce()

        let report = try XCTUnwrap(m.report)
        XCTAssertTrue(report.ok, "the kernel's own document verifies: \(report.message)")
        XCTAssertEqual(report.verified, 3)
        XCTAssertEqual(m.standing, .verified)
    }

    func testAnotherKeyThanThePinnedOneIsAnAlarmEvenWhenItsWalkPasses() async throws {
        // The log verifies — under a key this TV was never told to trust.
        // That is the one passing walk that must alarm, not reassure.
        let vector = try sharedSealedLogVector()
        let served = try XCTUnwrap(VerificationStanding.servedKey(in: vector))
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: vector)
        let m = pairedModel(transport)
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        await m.refreshOnce()

        XCTAssertEqual(m.report?.ok, true, "the walk itself is kept, not hidden")
        XCTAssertEqual(m.standing, .keyChanged(pinned: otherKey, served: served))
        XCTAssertTrue(m.standing.isAlarm)
    }

    func testForgettingDropsTheTokenAndThePinTogether() async {
        let secrets = MemoryPairingSecrets()
        let transport = SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: tamperedSealedLog)
        let m = pairedModel(transport, secrets: secrets)
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey)))
        await m.refreshOnce()

        m.forgetPairing()
        XCTAssertNil(m.pairing)
        XCTAssertEqual(secrets.accounts, [], "one item held both; nothing is left of either")
        XCTAssertEqual(m.standing, .none)
        XCTAssertNil(m.report)

        await m.refreshOnce()
        XCTAssertEqual(transport.tokensSent.last, .some(nil), "a forgotten pairing sends no token")
        XCTAssertEqual(m.standing, .unpaired)
    }

    func testAReceiptThatCannotPairSavesNothing() {
        let secrets = MemoryPairingSecrets()
        let m = pairedModel(SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: nil), secrets: secrets)

        XCTAssertNotNil(m.pair(receiptText: "not a receipt"))
        XCTAssertNotNil(m.pair(receiptText: #"{"sealed_log_token":"\#(viewerToken)"}"#),
                        "no verifying key, nothing to pin — refused")
        XCTAssertEqual(secrets.accounts, [])
        XCTAssertNil(m.pairing)
    }

    func testPairingNeedsOneSourceUnlessTheReceiptNamesIt() {
        let secrets = MemoryPairingSecrets()
        let m = pairedModel(SealedLogTransport(fleet: [.success(goodFleet)], sealedLog: nil),
                            secrets: secrets, sources: ["a.local", "b.local"])

        let problem = m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey))
        XCTAssertEqual(problem, PairingError.noSingleSource.localizedDescription,
                       "one pin cannot vouch for a merged wall")
        XCTAssertEqual(secrets.accounts, [])

        // A receipt that names its hub pairs it and points the Wall there.
        XCTAssertNil(m.pair(receiptText: receiptJSON(token: viewerToken, key: otherKey,
                                                     baseURL: "http://192.168.1.20:8799")))
        m.stop()
        XCTAssertEqual(m.sources, ["http://192.168.1.20:8799"])
        XCTAssertEqual(secrets.accounts, ["http://192.168.1.20:8799"])
        XCTAssertEqual(m.pairing?.verifyingKey, otherKey)
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

    func testTheSealedLogEndpointSitsBesideTheFleetEndpoint() throws {
        let base = try FleetAddress.normalize("canary.local:8099")
        XCTAssertEqual(FleetAddress.sealedLogEndpoint(for: base).path, "/api/sealed-log")
    }

    func testPastingTheFullFleetURLStillFindsTheSealedLog() throws {
        let base = try FleetAddress.normalize("http://canary.local:8099/api/fleet")
        XCTAssertEqual(FleetAddress.sealedLogEndpoint(for: base).path, "/api/sealed-log")
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
