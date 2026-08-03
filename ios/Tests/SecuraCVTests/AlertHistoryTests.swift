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
}
