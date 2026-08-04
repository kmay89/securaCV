// MuteLedgerTests.swift
//
// The durable half of mute, and the promise it must never weaken: a mute
// survives the 20-second row rebuild, expires on its own, prunes itself —
// and tamper still punches through, because that guarantee lives in
// Witness.effectiveSeverity where no ledger path can reach it.

import XCTest
@testable import SecuraCV

final class MuteLedgerTests: XCTestCase {
    private func freshLedger() throws -> (MuteLedger, UserDefaults, String) {
        let suite = "test-mute-ledger-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        return (MuteLedger(defaults: defaults), defaults, suite)
    }

    func testAMuteSurvivesTheRowRebuild() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        let until = Date().addingTimeInterval(3600)
        ledger.set(until: until, for: "canary-a")

        // Rows rebuilt from scratch, as every refresh does:
        var rebuilt = [Witness(id: "canary-a"), Witness(id: "canary-b")]
        ledger.apply(to: &rebuilt)
        XCTAssertNotNil(rebuilt[0].mutedUntil)
        XCTAssertTrue(rebuilt[0].isMuted)
        XCTAssertNil(rebuilt[1].mutedUntil)
    }

    func testAnExpiredMuteVanishesAndPrunesItself() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.set(until: Date().addingTimeInterval(-60), for: "canary-a")
        var rows = [Witness(id: "canary-a")]
        ledger.apply(to: &rows)
        XCTAssertNil(rows[0].mutedUntil)
        XCTAssertNil(ledger.muteUntil(for: "canary-a"))
        // Pruned from storage, not merely skipped:
        let stored = defaults.dictionary(forKey: MuteLedger.key) as? [String: Double]
        XCTAssertTrue(stored?.isEmpty ?? true)
    }

    func testClearUnmutesImmediately() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-a")
        ledger.clear("canary-a")
        var rows = [Witness(id: "canary-a")]
        ledger.apply(to: &rows)
        XCTAssertNil(rows[0].mutedUntil)
    }

    func testTamperPunchesThroughAMuteAppliedByTheLedger() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-a")
        var w = Witness(id: "canary-a")
        w.tamper = true
        var rows = [w]
        ledger.apply(to: &rows)
        XCTAssertTrue(rows[0].isMuted)
        XCTAssertEqual(rows[0].effectiveSeverity, .tamper,
                       "mute quiets nagging, never the smoke alarm")
    }

    // MARK: - the fleet-wide verbs (Quiet Hour / Resume Alerts)

    func testActiveMutesListsOnlyUnexpiredEntries() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-a")
        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-b")
        ledger.set(until: Date().addingTimeInterval(-60), for: "canary-expired")
        XCTAssertEqual(Set(ledger.activeMutes()), ["canary-a", "canary-b"])
    }

    func testClearAllReturnsEveryWitnessToFullVolume() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-a")
        ledger.set(until: Date().addingTimeInterval(3600), for: "canary-b")
        ledger.clearAll()
        XCTAssertTrue(ledger.activeMutes().isEmpty)
        var rows = [Witness(id: "canary-a"), Witness(id: "canary-b")]
        ledger.apply(to: &rows)
        XCTAssertTrue(rows.allSatisfy { $0.mutedUntil == nil })
    }
}
