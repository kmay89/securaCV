// AlertPolicyTests.swift
//
// The four decisions that stand between the fleet and the user's attention:
// when an unanswered alarm may buzz again (EscalationPolicy), when a window
// of quiet may hold one (QuietHours), how far a single Canary may be narrowed
// (WitnessPushFloor), and when the app may offer to stop pushing a whole
// class (AlertTuning). Each has one rule it may never break, and each of
// those rules gets a test named after it.

import XCTest
@testable import SecuraCV

final class AlertPolicyTests: XCTestCase {

    // MARK: - escalation is rationed to the top tier

    func testOnlyTheTopTierEscalates() {
        let old = Date().addingTimeInterval(-3600)
        for severity in [Severity.ok, .notice, .warn, .alert] {
            XCTAssertFalse(EscalationPolicy.shouldEscalate(severity: severity,
                                                           integrityFailed: false,
                                                           firstPosted: old, now: Date(),
                                                           acknowledged: false,
                                                           alreadyEscalated: false),
                           "\(severity) must never escalate — rationing is what keeps the second buzz meaning something")
        }
        XCTAssertTrue(EscalationPolicy.shouldEscalate(severity: .tamper, integrityFailed: false,
                                                      firstPosted: old, now: Date(),
                                                      acknowledged: false, alreadyEscalated: false))
    }

    func testAFailedSignatureEscalatesEvenThoughItSitsAtAlert() {
        let old = Date().addingTimeInterval(-3600)
        XCTAssertTrue(EscalationPolicy.shouldEscalate(severity: .alert, integrityFailed: true,
                                                      firstPosted: old, now: Date(),
                                                      acknowledged: false, alreadyEscalated: false),
                      "someone interfering with the witness is the same class of news as tamper")
    }

    func testAnAcknowledgedAlarmNeverEscalates() {
        let old = Date().addingTimeInterval(-3600)
        XCTAssertFalse(EscalationPolicy.shouldEscalate(severity: .tamper, integrityFailed: false,
                                                       firstPosted: old, now: Date(),
                                                       acknowledged: true, alreadyEscalated: false),
                       "answering it is the whole point of having an ack")
    }

    func testItEscalatesAtMostOnce() {
        let old = Date().addingTimeInterval(-3600)
        XCTAssertFalse(EscalationPolicy.shouldEscalate(severity: .tamper, integrityFailed: false,
                                                       firstPosted: old, now: Date(),
                                                       acknowledged: false, alreadyEscalated: true))
    }

    func testItNeverFiresEarlyOffACoarseTimestamp() {
        let now = Date()
        // Every time this policy can read is a 10-minute bucket floored DOWN,
        // so a record stamped "5 minutes ago" may really be seconds old.
        // Waiting only `delay` would escalate instantly at a bucket boundary.
        let justInsideTheBucket = now.addingTimeInterval(-(EscalationPolicy.delay + 60))
        XCTAssertFalse(EscalationPolicy.shouldEscalate(severity: .tamper, integrityFailed: false,
                                                       firstPosted: justInsideTheBucket, now: now,
                                                       acknowledged: false, alreadyEscalated: false),
                       "a bucketed stamp must be paid back in full before the second buzz")
        let past = now.addingTimeInterval(-(EscalationPolicy.delay + EscalationPolicy.bucketWidth + 1))
        XCTAssertTrue(EscalationPolicy.shouldEscalate(severity: .tamper, integrityFailed: false,
                                                      firstPosted: past, now: now,
                                                      acknowledged: false, alreadyEscalated: false))
    }

    // MARK: - quiet hours never quiet the smoke alarm

    private var calendar: Calendar {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "America/New_York")!
        return cal
    }

    private func at(_ hour: Int, _ minute: Int = 0) throws -> Date {
        try XCTUnwrap(calendar.date(from: DateComponents(year: 2026, month: 8, day: 14,
                                                         hour: hour, minute: minute)))
    }

    func testCriticalAlwaysPasses() throws {
        var hours = QuietHours()
        hours.enabled = true                       // 22:00 → 07:00 by default
        let threeAM = try at(3)
        XCTAssertTrue(hours.silences(.important, at: threeAM, calendar: calendar))
        XCTAssertFalse(hours.silences(.critical, at: threeAM, calendar: calendar),
                       "a smoke alarm that honors quiet hours is a decoration")
    }

    func testTheWindowWrapsMidnight() throws {
        var hours = QuietHours()
        hours.enabled = true
        XCTAssertTrue(hours.contains(try at(23), calendar: calendar))
        XCTAssertTrue(hours.contains(try at(2), calendar: calendar))
        XCTAssertFalse(hours.contains(try at(7), calendar: calendar), "the end is exclusive")
        XCTAssertFalse(hours.contains(try at(12), calendar: calendar))
    }

    func testADaytimeWindowDoesNotWrap() throws {
        var hours = QuietHours(enabled: true, startHour: 9, startMinute: 0,
                               endHour: 17, endMinute: 0)
        XCTAssertTrue(hours.contains(try at(12), calendar: calendar))
        XCTAssertFalse(hours.contains(try at(20), calendar: calendar))
        hours.enabled = false
        XCTAssertFalse(hours.contains(try at(12), calendar: calendar), "off means off")
    }

    func testAZeroLengthWindowSilencesNothing() throws {
        let hours = QuietHours(enabled: true, startHour: 22, startMinute: 0,
                               endHour: 22, endMinute: 0)
        for hour in 0..<24 {
            XCTAssertFalse(hours.contains(try at(hour), calendar: calendar),
                           "an empty window must never read as ‘always’")
        }
    }

    func testPickersRoundTripWallClockTime() throws {
        var hours = QuietHours()
        hours.setStart(try at(23, 30), calendar: calendar)
        hours.setEnd(try at(6, 15), calendar: calendar)
        XCTAssertEqual(hours.startHour, 23)
        XCTAssertEqual(hours.startMinute, 30)
        XCTAssertEqual(hours.endHour, 6)
        XCTAssertEqual(hours.endMinute, 15)
    }

    // MARK: - a per-witness floor narrows, and can never silence

    func testEveryFloorStillLetsTamperThrough() {
        for floor in WitnessPushFloor.allCases {
            XCTAssertLessThanOrEqual(floor.minSeverity, .tamper,
                                     "there is deliberately no ‘never’: tamper reaches you from every Canary")
        }
    }

    func testTheFloorLadderNarrows() {
        XCTAssertEqual(WitnessPushFloor.armed.minSeverity, .ok)
        XCTAssertEqual(WitnessPushFloor.serious.minSeverity, .alert)
        XCTAssertEqual(WitnessPushFloor.tamperOnly.minSeverity, .tamper)
    }

    func testTheFloorPersistsAndTheDefaultIsStoredAsAbsence() throws {
        let suite = "test-witness-floor-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        let prefs = WitnessAlertPrefs(defaults: defaults)
        XCTAssertEqual(prefs.floor(for: "canary-a3f7"), .armed)
        prefs.set(.serious, for: "canary-a3f7")
        XCTAssertEqual(WitnessAlertPrefs(defaults: defaults).floor(for: "canary-a3f7"), .serious)
        XCTAssertEqual(prefs.narrowedIDs(), ["canary-a3f7"])

        prefs.set(.armed, for: "canary-a3f7")
        XCTAssertTrue(prefs.narrowedIDs().isEmpty,
                      "back to the default is not a ‘choice’ the rules sheet should count")
    }

    // MARK: - the app may offer to stop pushing; it may never just do it

    private func rules() -> [AlertRule] { AlertRule.defaults }

    func testNoAdviceWithoutEnoughEvidence() {
        var stats = AlertActionStats()
        for _ in 0..<(AlertTuning.minimumSample - 1) { stats.recordDismissed(.alert) }
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: rules(), declined: []),
                     "a handful of dismissals on a quiet week is not evidence")
    }

    func testAdviceWhenAClassIsAlmostAlwaysDismissed() throws {
        var stats = AlertActionStats()
        for _ in 0..<19 { stats.recordDismissed(.alert) }
        stats.recordActed(.alert)
        let advice = try XCTUnwrap(AlertTuning.advice(stats: stats, rules: rules(), declined: []))
        XCTAssertEqual(advice.dismissed, 19)
        XCTAssertEqual(advice.total, 20)
    }

    func testTheOfferNamesEveryRuleThatWouldKeepPushingTheClass() throws {
        var stats = AlertActionStats()
        for _ in 0..<20 { stats.recordDismissed(.alert) }
        let advice = try XCTUnwrap(AlertTuning.advice(stats: stats, rules: rules(), declined: []))
        XCTAssertEqual(Set(advice.ruleIDs), ["integrity", "dark"],
                       "the shipped rules OVERLAP on an alarm — switching off one would leave the "
                       + "other pushing exactly what the button promised to stop")
        XCTAssertFalse(advice.ruleIDs.contains("tamper"), "a more serious class is not this offer's to touch")
    }

    func testAClassThatNeverPushesIsNeverOfferedForDemotion() {
        var stats = AlertActionStats()
        for _ in 0..<20 { stats.recordDismissed(.notice) }
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: rules(), declined: []),
                     "everyday activity is pull-only; offering to stop pushing it would be theater")
    }

    func testAClassTheUserActsOnIsNeverOfferedForDemotion() {
        var stats = AlertActionStats()
        for _ in 0..<10 { stats.recordDismissed(.alert) }
        for _ in 0..<10 { stats.recordActed(.alert) }
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: rules(), declined: []))
    }

    func testTheSmokeAlarmIsNotTunable() {
        var stats = AlertActionStats()
        for _ in 0..<40 { stats.recordDismissed(.tamper) }
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: rules(), declined: []),
                     "however often tamper gets dismissed, the app never offers to stop pushing it")
    }

    func testDecliningIsRemembered() throws {
        var stats = AlertActionStats()
        for _ in 0..<20 { stats.recordDismissed(.alert) }
        let advice = try XCTUnwrap(AlertTuning.advice(stats: stats, rules: rules(), declined: []))
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: rules(), declined: [advice.id]),
                     "‘keep them’ must not become its own nag")
        XCTAssertEqual(advice.id, "sev-\(Severity.alert.rawValue)",
                       "declining is an answer about a CLASS, so it has to outlive any one rule")
    }

    func testRulesAlreadyOffAreNeverOffered() {
        var stats = AlertActionStats()
        for _ in 0..<20 { stats.recordDismissed(.alert) }
        var off = AlertRule.defaults
        for i in off.indices where off[i].minSeverity == .alert { off[i].enabled = false }
        XCTAssertNil(AlertTuning.advice(stats: stats, rules: off, declined: []),
                     "advising someone to turn off something already off is how ‘smart’ loses trust")
    }

    func testCountersPersistAndCanBeForgotten() throws {
        let suite = "test-alert-tuning-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }

        let ledger = AlertTuningLedger(defaults: defaults)
        ledger.recordDismissed(.notice)
        ledger.recordDismissed(.notice)
        ledger.recordActed(.notice)
        XCTAssertEqual(AlertTuningLedger(defaults: defaults).stats.total(.notice), 3)

        ledger.decline(key: "sev-3")
        XCTAssertTrue(AlertTuningLedger(defaults: defaults).declined.contains("sev-3"))

        ledger.forget(.notice)
        XCTAssertEqual(AlertTuningLedger(defaults: defaults).stats.total(.notice), 0,
                       "a re-armed rule is judged on what happens next, not on what got it turned off")
    }

    // MARK: - the rules themselves

    func testTheStrongestRuleDecides() throws {
        let strongest = try XCTUnwrap(AlertRule.strongest(for: .tamper, in: AlertRule.defaults))
        XCTAssertEqual(strongest.id, "tamper")
        XCTAssertNil(AlertRule.strongest(for: .ok, in: AlertRule.defaults))
    }

    func testMergeKeepsTheUsersChoicesAndTheAppsWords() {
        var stored = AlertRule.defaults
        for i in stored.indices where stored[i].id == "activity" {
            stored[i].enabled = false
            stored[i].reach = .anywhere
            stored[i].title = "an old build's wording"
        }
        let merged = AlertRule.merge(stored: stored)
        let activity = merged.first { $0.id == "activity" }
        XCTAssertEqual(activity?.enabled, false, "what the user armed is theirs")
        XCTAssertEqual(activity?.reach, .anywhere)
        XCTAssertEqual(activity?.title, "Everyday activity",
                       "the wording belongs to the app, so improving it reaches everyone")
        XCTAssertEqual(merged.count, AlertRule.defaults.count)
    }

    func testMergeSurvivesARuleThatNoLongerExists() {
        let stored = [AlertRule(id: "retired-rule", title: "gone", minSeverity: .warn)]
        XCTAssertEqual(AlertRule.merge(stored: stored), AlertRule.defaults,
                       "a rule this version doesn't ship simply isn't one")
    }
}
