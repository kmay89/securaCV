// AlertFreshnessTests.swift
//
// The lifecycle's user-facing edges: the all-clear that keeps silence from
// being the only "it's fine" signal, the "while you were away" line that
// makes recovery explicit instead of leaving it to scrolling, and the two
// stamps a row can now carry (when a mute ends, and whether nobody answered).
//
// The rule these tests exist to hold: a mute or an escalation belongs to ONE
// occurrence. A condition that comes back is news again — new silence, new
// urgency, nothing inherited from the last time.

import XCTest
@testable import SecuraCV

@MainActor
final class AlertFreshnessTests: XCTestCase {
    private func freshLedger() throws -> (AlertLedger, UserDefaults, String) {
        let suite = "test-alert-freshness-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        return (AlertLedger(defaults: defaults), defaults, suite)
    }

    private func record(id: String = "canary-a3f7|3|Gone dark",
                        witness: String = "canary-a3f7",
                        name: String = "Front Porch",
                        headline: String = "Gone dark",
                        severity: Severity = .alert,
                        at date: Date = Date()) -> AlertRecord {
        AlertRecord(id: id, witnessID: witness, name: name,
                    severity: severity, headline: headline, at: date)
    }

    // MARK: - the all-clear earns a line in Today

    func testAResolvedConditionBecomesATimelineEntry() {
        var r = record()
        r.resolvedBucket = AlertRecord.bucket(for: Date())
        let events = AlertFreshness.allClearEvents([r])
        XCTAssertEqual(events.count, 1)
        XCTAssertEqual(events[0].headline, "Front Porch — Gone dark, now clear")
        XCTAssertEqual(events[0].severity, .ok)
        XCTAssertEqual(events[0].timeBucket, r.resolvedBucket)
    }

    func testALiveConditionIsNotAnAllClear() {
        XCTAssertTrue(AlertFreshness.allClearEvents([record()]).isEmpty,
                      "nothing has cleared yet — that's the whole distinction")
    }

    func testTheAllClearIsNeverASignedDeviceClaim() {
        var r = record()
        r.resolvedBucket = AlertRecord.bucket(for: Date())
        XCTAssertEqual(AlertFreshness.allClearEvents([r])[0].badge, .unknown,
                       "this line is the phone's own observation, not one of the fleet's witnessed records")
    }

    func testYesterdaysAllClearHasLeftTodaysTimeline() {
        var r = record()
        r.resolvedBucket = AlertRecord.bucket(for: Date().addingTimeInterval(-2 * 86_400))
        XCTAssertTrue(AlertFreshness.allClearEvents([r]).isEmpty,
                      "Today is a day's story; older clears live in the Alerts tab's history")
    }

    func testAllClearIDsCannotCollideWithWitnessEvents() {
        var r = record()
        r.resolvedBucket = AlertRecord.bucket(for: Date())
        XCTAssertTrue(AlertFreshness.allClearEvents([r])[0].id.hasPrefix("allclear#"))
    }

    // MARK: - "while you were away"

    func testTheSummaryCountsWhatWasMissedAndWhatIsStillLive() throws {
        var live = record()
        var settled = record(id: "canary-b2|3|Gone dark", witness: "canary-b2", name: "Garage")
        settled.resolvedBucket = AlertRecord.bucket(for: Date())
        let line = try XCTUnwrap(AlertFreshness.awaySummary([live, settled]))
        XCTAssertEqual(line, "2 things happened while you were away — 1 still needs you.")

        live.seenBucket = AlertRecord.bucket(for: Date())
        settled.seenBucket = live.seenBucket
        XCTAssertNil(AlertFreshness.awaySummary([live, settled]),
                     "once looked at, there is nothing left to reconcile")
    }

    func testTheSummarySaysSoWhenItIsAllOver() throws {
        var settled = record()
        settled.resolvedBucket = AlertRecord.bucket(for: Date())
        XCTAssertEqual(AlertFreshness.awaySummary([settled]),
                       "1 thing happened while you were away — all of it is over now.")
    }

    func testNothingUnseenMeansNoBanner() {
        var seen = record()
        seen.seenBucket = AlertRecord.bucket(for: Date())
        XCTAssertNil(AlertFreshness.awaySummary([seen]))
        XCTAssertNil(AlertFreshness.awaySummary([]))
    }

    // MARK: - a mute with a visible end

    func testMutingStampsWhenTheQuietRunsOut() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        let until = Date().addingTimeInterval(3600)
        ledger.mark(.muted, forWitness: "canary-a3f7", until: until)

        let row = try XCTUnwrap(ledger.records.first)
        XCTAssertEqual(row.handling, .muted)
        XCTAssertEqual(row.mutedUntil, AlertRecord.bucket(for: until),
                       "coarse like every other time this record knows (Invariant III)")
    }

    func testAConditionThatComesBackIsNotStillMuted() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        ledger.mark(.muted, forWitness: "canary-a3f7", until: Date().addingTimeInterval(3600))
        ledger.markEscalated(id: "canary-a3f7|3|Gone dark")

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        let row = try XCTUnwrap(ledger.records.first)
        XCTAssertNil(row.mutedUntil, "a repeat reopens the whole lifecycle")
        XCTAssertFalse(row.wasEscalated, "and it may be escalated again on its own merits")
        XCTAssertEqual(row.handling, .new)
    }

    // MARK: - escalation stamps once

    func testEscalationIsStampedOnlyOnce() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|4|Tamper detected", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .tamper, headline: "Tamper detected")
        let first = Date(timeIntervalSince1970: 1_800_000_000)
        ledger.markEscalated(id: "canary-a3f7|4|Tamper detected", now: first)
        ledger.markEscalated(id: "canary-a3f7|4|Tamper detected",
                             now: first.addingTimeInterval(9999))
        XCTAssertEqual(ledger.records[0].escalatedBucket, AlertRecord.bucket(for: first))
    }

    func testEscalationSurvivesARelaunch() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|4|Tamper detected", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .tamper, headline: "Tamper detected")
        ledger.markEscalated(id: "canary-a3f7|4|Tamper detected")

        let reloaded = AlertLedger(defaults: defaults)
        XCTAssertTrue(reloaded.records[0].wasEscalated,
                      "‘at most once’ has to outlive the app, or a cold start re-buzzes every alarm")
    }

    // MARK: - old ledgers still decode

    func testALedgerWrittenBeforeTheseFieldsDecodesUnchanged() throws {
        let json = """
        [{"id":"canary-a3f7|3|Gone dark","witnessID":"canary-a3f7","name":"Front Porch",
          "severityRaw":3,"headline":"Gone dark","bucket":774144000,"lastBucket":774144000,
          "count":1,"deliveryRaw":1,"handlingRaw":0}]
        """
        let decoded = try JSONDecoder().decode([AlertRecord].self, from: Data(json.utf8))
        XCTAssertEqual(decoded.count, 1)
        XCTAssertNil(decoded[0].mutedUntil)
        XCTAssertNil(decoded[0].escalatedBucket)
        XCTAssertTrue(decoded[0].isOpen)
    }
}
