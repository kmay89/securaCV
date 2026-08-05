// AlertHistoryTests.swift
//
// The Alerts tab's two promises, pinned: the list stays readable (repeats
// collapse) and it never overstates reach (delivery is recorded, only ever
// climbs, and an undelivered alert says why). Plus the wake payload's one
// rule — a wake carries a coarse class and nothing else — checked against
// both envelope shapes so the CloudKit path today and a self-hosted relay
// later decode identically.

import XCTest
@testable import SecuraCV

@MainActor
final class AlertHistoryTests: XCTestCase {
    private func freshLedger() throws -> (AlertLedger, UserDefaults, String) {
        let suite = "test-alert-ledger-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        return (AlertLedger(defaults: defaults), defaults, suite)
    }

    // MARK: - collapse (a list you can read at 3am)

    func testRepeatsCollapseIntoOneRowWithACount() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        for _ in 0..<6 {
            ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                        name: "Front Porch", severity: .alert, headline: "Gone dark")
        }
        XCTAssertEqual(ledger.records.count, 1, "a flapping Canary is one line, not six")
        XCTAssertEqual(ledger.records[0].count, 6)
    }

    func testADifferentConditionIsANewRow() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        ledger.note(id: "canary-a3f7|4|Tamper detected", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .tamper, headline: "Tamper detected")
        XCTAssertEqual(ledger.records.count, 2, "a different condition is different news")
        XCTAssertEqual(ledger.records[0].severity, .tamper, "newest first")
    }

    func testAnAcknowledgedConditionThatReturnsIsNewsAgain() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        ledger.mark(.acknowledged, forWitness: "canary-a3f7")
        XCTAssertEqual(ledger.unhandledCount, 0)

        ledger.note(id: "canary-a3f7|3|Gone dark", witnessID: "canary-a3f7",
                    name: "Front Porch", severity: .alert, headline: "Gone dark")
        XCTAssertEqual(ledger.unhandledCount, 1,
                       "it came BACK — an old acknowledgment doesn't cover a new occurrence")
    }

    // MARK: - reach honesty

    func testDeliveryOnlyEverClimbs() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        let r = ledger.note(id: "k", witnessID: "w", name: "Garage",
                            severity: .alert, headline: "Gone dark")
        ledger.markDelivery(.away, for: r.id)
        ledger.markDelivery(.onLAN, for: r.id)
        XCTAssertEqual(ledger.records[0].delivery, .away,
                       "a wake already reached the pocket; a later local post must not downgrade the record")
        ledger.markDelivery(.notDelivered, for: r.id, reason: "Notifications are off.")
        XCTAssertEqual(ledger.records[0].delivery, .away)
    }

    func testAnUndeliveredAlertKeepsItsReason() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        let r = ledger.note(id: "k", witnessID: "w", name: "Garage",
                            severity: .alert, headline: "Gone dark")
        ledger.markDelivery(.notDelivered, for: r.id, reason: "Notifications are off for SecuraCV.")
        XCTAssertEqual(ledger.records[0].delivery, .notDelivered)
        XCTAssertEqual(ledger.records[0].undeliveredReason, "Notifications are off for SecuraCV.",
                       "'we couldn't tell you' is useless without the reason")
    }

    func testTheLedgerSurvivesARelaunch() throws {
        let suite = "test-alert-ledger-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        let first = AlertLedger(defaults: defaults)
        first.note(id: "k", witnessID: "w", name: "Garage",
                   severity: .alert, headline: "Gone dark")
        let second = AlertLedger(defaults: defaults)
        XCTAssertEqual(second.records.count, 1, "history that forgets on relaunch isn't history")
    }

    // MARK: - Invariant III on the record itself

    func testRecordsKeepOnlyACoarseBucket() {
        let precise = Date(timeIntervalSince1970: 1_784_000_557)   // :09:17
        let record = AlertRecord(id: "k", witnessID: "w", name: "Garage",
                                 severity: .alert, headline: "Gone dark", at: precise)
        XCTAssertEqual(record.bucket.timeIntervalSince1970
                        .truncatingRemainder(dividingBy: 600), 0,
                       "no precise second ever lands in an alert record (Invariant III)")
        XCTAssertLessThanOrEqual(record.bucket, precise)
    }

    // MARK: - the wake carries a class and nothing else

    func testAWakeDecodesFromBothEnvelopes() {
        let flat: [AnyHashable: Any] = ["sev": "tamper"]
        XCTAssertEqual(WakePayload.wakeClass(from: flat), .tamper,
                       "a self-hosted relay's flat body (design doc §5)")

        let cloudKit: [AnyHashable: Any] = ["ck": ["qry": ["af": ["sev": "offline"]]]]
        XCTAssertEqual(WakePayload.wakeClass(from: cloudKit), .offline,
                       "CloudKit's query-notification envelope, shipped today")

        XCTAssertNil(WakePayload.wakeClass(from: ["ck": ["qry": [:]]]),
                     "a wake with no class is not guessed at")
    }

    func testOnlyTamperIsLifeSafety() {
        XCTAssertTrue(WakeClass.tamper.isLifeSafety)
        for wake in [WakeClass.integrity, .offline, .pattern] {
            XCTAssertFalse(wake.isLifeSafety, "\(wake) must not claim the smoke-alarm level")
        }
    }

    func testTheWakeClassLadderPicksTheWorstTruth() {
        XCTAssertEqual(WakeClass(severity: .tamper, badgeFailed: true, wentDark: true), .tamper)
        XCTAssertEqual(WakeClass(severity: .alert, badgeFailed: true, wentDark: true), .integrity,
                       "a broken proof outranks a dark link")
        XCTAssertEqual(WakeClass(severity: .alert, badgeFailed: false, wentDark: true), .offline)
        XCTAssertEqual(WakeClass(severity: .alert, badgeFailed: false, wentDark: false), .pattern)
    }

    // MARK: - per-rule reach governs what may leave the house

    func testOnlyTheMatchingRulesReachDecidesWhetherAWakeIsPublished() {
        let center = AlertCenter()
        // Everyday activity may travel; the serious rules are Wi-Fi only.
        center.rules = [
            .init(id: "dark", title: "A Canary went dark", minSeverity: .alert,
                  reach: .onWiFiOnly),
            .init(id: "activity", title: "Everyday activity", minSeverity: .notice,
                  reach: .anywhere),
        ]
        XCTAssertTrue(AlertRule.anyReachesAnywhere(rules: center.rules),
                      "something wants away reach, so the path is worth setting up")
        XCTAssertFalse(center.reachesAnywhere(severity: .alert),
                       "but the rule that MATCHES an alert says Wi-Fi only — an unrelated "
                       + "rule's Anywhere must not push this alarm out of the house")
        XCTAssertTrue(center.reachesAnywhere(severity: .notice),
                      "the rule that does match may travel")
    }

    func testReachUsesTheStrongestMatchingRuleJustLikeLevelDoes() {
        let center = AlertCenter()
        center.rules = [
            .init(id: "activity", title: "Everyday activity", minSeverity: .notice,
                  reach: .onWiFiOnly),
            .init(id: "tamper", title: "Tamper or panic", minSeverity: .tamper,
                  reach: .anywhere),
        ]
        XCTAssertTrue(center.reachesAnywhere(severity: .tamper),
                      "tamper matches both rules; the strongest one wins, exactly as in level(for:)")
        XCTAssertFalse(center.reachesAnywhere(severity: .notice))
    }

    func testADisabledRuleGrantsNoReach() {
        let center = AlertCenter()
        center.rules = [
            .init(id: "dark", title: "A Canary went dark", minSeverity: .alert,
                  reach: .anywhere, enabled: false),
        ]
        XCTAssertFalse(center.reachesAnywhere(severity: .alert))
    }

    // MARK: - the away path is only armed when a rule asks for it

    func testAwayPathIsOnlySetUpWhenARuleWantsIt() {
        XCTAssertTrue(AlertRule.anyReachesAnywhere(rules: AlertRule.defaults),
                      "the shipped defaults do arm away reach for the serious rules")
        let localOnly = AlertRule.defaults.map { rule -> AlertRule in
            var r = rule
            r.reach = .onWiFiOnly
            return r
        }
        XCTAssertFalse(AlertRule.anyReachesAnywhere(rules: localOnly),
                       "nobody gets registered for pushes they never asked to receive")
        let disabled = AlertRule.defaults.map { rule -> AlertRule in
            var r = rule
            r.enabled = false
            return r
        }
        XCTAssertFalse(AlertRule.anyReachesAnywhere(rules: disabled),
                       "a disabled rule arms nothing")
    }

    // MARK: - the lifecycle closes its loop (open → resolved, seen, aged out)

    func testResolutionClosesTheLoopWithoutTouchingHandling() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        XCTAssertTrue(ledger.records[0].isOpen)
        XCTAssertTrue(ledger.records[0].needsYou)

        ledger.resolve(witnessID: "w")
        XCTAssertFalse(ledger.records[0].isOpen, "the condition cleared")
        XCTAssertFalse(ledger.records[0].needsYou, "over is never urgent")
        XCTAssertEqual(ledger.records[0].handling, .new,
                       "clearing on its own is not an acknowledgment — the row still says nobody looked")
    }

    func testResolveIsScopedToTheWitness() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "a|3|Gone dark", witnessID: "a", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.note(id: "b|4|Tamper detected", witnessID: "b", name: "Porch",
                    severity: .tamper, headline: "Tamper detected")
        ledger.resolve(witnessID: "a")
        let byWitness = Dictionary(uniqueKeysWithValues: ledger.records.map { ($0.witnessID, $0) })
        XCTAssertFalse(try XCTUnwrap(byWitness["a"]).isOpen)
        XCTAssertTrue(try XCTUnwrap(byWitness["b"]).isOpen,
                      "the other Canary's alarm is still live")
    }

    func testAConditionThatReturnsReopensTheWholeLifecycle() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.resolve(witnessID: "w")
        ledger.markSeen()
        XCTAssertEqual(ledger.unseenCount, 0)

        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        XCTAssertTrue(ledger.records[0].isOpen, "it came back — open again")
        XCTAssertTrue(ledger.records[0].isUnseen, "and unseen again: the return is news")
    }

    func testMarkSeenClearsTheBadgeCountNotTheHandling() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        XCTAssertEqual(ledger.unseenCount, 1)
        ledger.markSeen()
        XCTAssertEqual(ledger.unseenCount, 0)
        XCTAssertEqual(ledger.unhandledCount, 1,
                       "glancing at a live alarm does not acknowledge it")
    }

    func testTheSweepTakesOnlySeenAndSettledHistory() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        let old = Date().addingTimeInterval(-AlertLedger.retention - 86_400)
        // Three month-old rows in the three states that matter.
        ledger.note(id: "seen|3|Gone dark", witnessID: "seen", name: "Garage",
                    severity: .alert, headline: "Gone dark", now: old)
        ledger.note(id: "unseen|3|Gone dark", witnessID: "unseen", name: "Porch",
                    severity: .alert, headline: "Gone dark", now: old)
        ledger.note(id: "live|3|Gone dark", witnessID: "live", name: "Shed",
                    severity: .alert, headline: "Gone dark", now: old)
        ledger.resolve(witnessID: "seen", now: old)
        ledger.resolve(witnessID: "unseen", now: old)
        // markSeen stamps every row present, so build the unseen fixture by
        // re-noting that condition afterward — a return wipes the seen stamp
        // (the reopen rule) — and resolving it again.
        ledger.markSeen(now: old)
        ledger.note(id: "unseen|3|Gone dark", witnessID: "unseen", name: "Porch",
                    severity: .alert, headline: "Gone dark", now: old)
        ledger.resolve(witnessID: "unseen", now: old)

        ledger.retentionSweep()
        let ids = Set(ledger.records.map(\.witnessID))
        XCTAssertFalse(ids.contains("seen"),
                       "settled, seen, and a month stale — the sweep lets it go")
        XCTAssertTrue(ids.contains("unseen"),
                      "never silently delete something the user never saw")
        XCTAssertTrue(ids.contains("live"),
                      "time may not delete a live condition")
    }

    func testClearHistoryNeverTakesALiveAlarm() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "live|4|Tamper detected", witnessID: "live", name: "Porch",
                    severity: .tamper, headline: "Tamper detected")
        ledger.note(id: "done|3|Gone dark", witnessID: "done", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.resolve(witnessID: "done")

        ledger.clearSettled()
        XCTAssertEqual(ledger.records.map(\.witnessID), ["live"],
                       "an ongoing alarm can be acked or muted, never made to vanish — "
                       + "clearing it would also stay cleared, since the dedupe still "
                       + "holds its fingerprint and nothing would re-create the row")
    }

    func testASupersededConditionResolvesBeforeItsReplacementOpens() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        // The FleetStore sequence when a witness escalates without a calm
        // gap (dark → tamper, still live throughout): resolve the old story,
        // then note the new one.
        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.resolve(witnessID: "w")
        ledger.note(id: "w|4|Tamper detected", witnessID: "w", name: "Garage",
                    severity: .tamper, headline: "Tamper detected")

        let byID = Dictionary(uniqueKeysWithValues: ledger.records.map { ($0.id, $0) })
        XCTAssertFalse(try XCTUnwrap(byID["w|3|Gone dark"]).isOpen,
                       "the superseded record must not sit Ongoing forever, exempt from retention")
        XCTAssertTrue(try XCTUnwrap(byID["w|4|Tamper detected"]).isOpen,
                      "the new condition is the live one")
    }

    func testRemoveTakesExactlyOneRow() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "a|3|Gone dark", witnessID: "a", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.note(id: "b|3|Gone dark", witnessID: "b", name: "Porch",
                    severity: .alert, headline: "Gone dark")
        ledger.remove(id: "a|3|Gone dark")
        XCTAssertEqual(ledger.records.map(\.witnessID), ["b"])
    }

    // MARK: - relaunch fold (an alarm that outlives the app is not news)

    func testFoldRebuildsTheDedupeStateFromOpenRecords() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "a|3|Gone dark", witnessID: "a", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.note(id: "b|4|Tamper detected", witnessID: "b", name: "Porch",
                    severity: .tamper, headline: "Tamper detected")
        ledger.mark(.acknowledged, forWitness: "b")
        ledger.note(id: "c|3|Gone dark", witnessID: "c", name: "Shed",
                    severity: .alert, headline: "Gone dark")
        ledger.resolve(witnessID: "c")

        let (posted, acked) = AlertLedger.foldOpenAlerts(records: ledger.records)
        XCTAssertEqual(posted["a"], "3|Gone dark",
                       "the ongoing alarm must not re-post after a relaunch")
        XCTAssertEqual(posted["b"], "4|Tamper detected")
        XCTAssertEqual(acked["b"], "4|Tamper detected",
                       "an acknowledgment survives the relaunch too")
        XCTAssertNil(acked["a"])
        XCTAssertNil(posted["c"],
                     "a resolved condition folds nothing — its next alert IS news")
    }

    func testFoldPrefersTheNewestOpenRecordPerWitness() throws {
        let (ledger, defaults, suite) = try freshLedger()
        defer { defaults.removePersistentDomain(forName: suite) }

        ledger.note(id: "w|3|Gone dark", witnessID: "w", name: "Garage",
                    severity: .alert, headline: "Gone dark")
        ledger.note(id: "w|4|Tamper detected", witnessID: "w", name: "Garage",
                    severity: .tamper, headline: "Tamper detected")
        let (posted, _) = AlertLedger.foldOpenAlerts(records: ledger.records)
        XCTAssertEqual(posted["w"], "4|Tamper detected",
                       "newest condition wins, matching the live dedupe rule")
    }

    // MARK: - history shape (day sections, wrist cap)

    func testDaySectionsGroupByLocalDayNewestFirst() {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "America/Chicago")!
        let now = Date(timeIntervalSince1970: 1_784_000_000)
        let yesterday = now.addingTimeInterval(-86_400)

        var today1 = AlertRecord(id: "a|x", witnessID: "a", name: "Garage",
                                 severity: .alert, headline: "x", at: now)
        var today2 = AlertRecord(id: "b|x", witnessID: "b", name: "Porch",
                                 severity: .alert, headline: "x",
                                 at: now.addingTimeInterval(-3_600))
        var old = AlertRecord(id: "c|x", witnessID: "c", name: "Shed",
                              severity: .alert, headline: "x", at: yesterday)
        // Buckets are set from `at:` already; silence the "never mutated"
        // warning by resolving them, which is also the realistic state.
        today1.resolvedBucket = today1.lastBucket
        today2.resolvedBucket = today2.lastBucket
        old.resolvedBucket = old.lastBucket

        let sections = AlertHistory.daySections([old, today2, today1], calendar: cal)
        XCTAssertEqual(sections.count, 2)
        XCTAssertEqual(sections[0].records.map(\.witnessID), ["a", "b"],
                       "newest day first, newest row first within the day")
        XCTAssertEqual(sections[1].records.map(\.witnessID), ["c"])
        XCTAssertLessThan(sections[1].day, sections[0].day)
    }

    func testWristRowsNeverDropALiveAlarmBehindSettledHistory() {
        let now = Date()
        var rows: [AlertRecord] = []
        for i in 0..<12 {
            var settled = AlertRecord(id: "s\(i)|x", witnessID: "s\(i)", name: "Old",
                                      severity: .alert, headline: "x",
                                      at: now.addingTimeInterval(Double(-i) * 3_600))
            settled.resolvedBucket = settled.lastBucket
            rows.append(settled)
        }
        let live = AlertRecord(id: "live|x", witnessID: "live", name: "Now",
                               severity: .tamper, headline: "x",
                               at: now.addingTimeInterval(-13 * 3_600))
        rows.append(live)   // oldest by time, but still open and unhandled

        let wrist = AlertHistory.wristRows(rows, cap: 12)
        XCTAssertEqual(wrist.count, 12)
        XCTAssertEqual(wrist.first?.witnessID, "live",
                       "the one row that still needs a human always makes the cut")
    }
}
