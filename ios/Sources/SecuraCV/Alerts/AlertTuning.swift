// AlertTuning.swift
//
// The app notices when it is being annoying, and offers to stop.
//
// Every vendor measures this and none of them act on it for you: if you
// dismiss 95% of one class of alert, that class is noise, and the honest
// response is to stop pushing it rather than to wait for you to give up on
// notifications entirely (which is what people actually do — the clinical
// literature puts override rates for interruptive alerts at 49–96%).
//
// Three constraints make this a feature rather than a creepy one:
//   1. IT ONLY EVER OFFERS. Nothing here changes a rule on its own. The app
//      may say "you dismiss almost all of these — stop pushing them?"; the
//      answer is the user's, and "keep pushing" is remembered so it doesn't
//      ask again for that rule.
//   2. THE DATA NEVER LEAVES THE PHONE (Invariant IV). Two integers per
//      severity in UserDefaults. There is no model, no upload, no profile —
//      the whole mechanism is a counter and one sentence.
//   3. IT COUNTS ANSWERS, NOT CONTENT. An ack is "this mattered"; a mute or
//      a swipe-away is "this didn't". Nothing looks at what happened, only
//      at what the user did about it.

import Foundation

/// The counters, per severity class.
struct AlertActionStats: Codable, Hashable, Sendable {
    /// severity raw value → count. Keyed by String because these ride in a
    /// plist and JSON dictionaries key by string anyway.
    var acted: [String: Int] = [:]
    var dismissed: [String: Int] = [:]

    func acted(_ severity: Severity) -> Int { acted[String(severity.rawValue)] ?? 0 }
    func dismissed(_ severity: Severity) -> Int { dismissed[String(severity.rawValue)] ?? 0 }
    func total(_ severity: Severity) -> Int { acted(severity) + dismissed(severity) }

    mutating func recordActed(_ severity: Severity) {
        acted[String(severity.rawValue), default: 0] += 1
    }

    mutating func recordDismissed(_ severity: Severity) {
        dismissed[String(severity.rawValue), default: 0] += 1
    }

    mutating func forget(_ severity: Severity) {
        acted[String(severity.rawValue)] = nil
        dismissed[String(severity.rawValue)] = nil
    }
}

/// Counters + the "don't ask me about this rule again" set, over UserDefaults.
struct AlertTuningLedger {
    static let statsKey = "alert_action_stats_v1"
    static let declinedKey = "alert_tuning_declined_v1"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var stats: AlertActionStats {
        guard let data = defaults.data(forKey: Self.statsKey),
              let decoded = try? JSONDecoder().decode(AlertActionStats.self, from: data) else {
            return AlertActionStats()
        }
        return decoded
    }

    var declined: Set<String> {
        Set(defaults.stringArray(forKey: Self.declinedKey) ?? [])
    }

    func recordActed(_ severity: Severity) {
        var s = stats
        s.recordActed(severity)
        save(s)
    }

    func recordDismissed(_ severity: Severity) {
        var s = stats
        s.recordDismissed(severity)
        save(s)
    }

    /// "Keep pushing these" — remembered, so the offer is made once per rule
    /// and never becomes its own nag.
    func decline(ruleID: String) {
        var d = declined
        d.insert(ruleID)
        defaults.set(Array(d).sorted(), forKey: Self.declinedKey)
    }

    /// The offer was taken: start the evidence over, so a rule that gets
    /// re-armed later is judged on what happens next, not on the history
    /// that got it turned off.
    func forget(_ severity: Severity) {
        var s = stats
        s.forget(severity)
        save(s)
    }

    private func save(_ s: AlertActionStats) {
        guard let data = try? JSONEncoder().encode(s) else { return }
        defaults.set(data, forKey: Self.statsKey)
    }
}

enum AlertTuning {
    /// Enough answers that the ratio means something. A handful of dismissals
    /// on a quiet week is not evidence of anything.
    static let minimumSample = 12
    /// "Almost all of them."
    static let dismissThreshold = 0.9

    struct Advice: Equatable, Identifiable {
        let ruleID: String
        let ruleTitle: String
        let severity: Severity
        let dismissed: Int
        let total: Int

        var id: String { ruleID }

        var sentence: String {
            "You've dismissed \(dismissed) of the last \(total) “\(ruleTitle)” alerts."
        }

        var question: String {
            "Stop pushing these? They'll still be recorded, and still show up here — they just won't interrupt you."
        }
    }

    /// The one offer worth making right now, if any. Only for a rule that is
    /// actually enabled and actually pushes — advising someone to turn off
    /// something already off is how "smart" features lose trust.
    static func advice(stats: AlertActionStats,
                       rules: [AlertRule],
                       declined: Set<String>) -> Advice? {
        // Least-serious first: if two classes qualify, the everyday noise is
        // what gets offered for demotion — nobody should be asked to stop
        // pushing tamper while an activity rule is the thing they actually
        // dismiss all day.
        for severity in Severity.allCases.sorted(by: <) {
            let total = stats.total(severity)
            guard total >= minimumSample else { continue }
            let dismissed = stats.dismissed(severity)
            guard Double(dismissed) / Double(total) >= dismissThreshold else { continue }
            guard let rule = AlertRule.strongest(for: severity, in: rules),
                  rule.enabled, !declined.contains(rule.id) else { continue }
            return Advice(ruleID: rule.id, ruleTitle: rule.title, severity: severity,
                          dismissed: dismissed, total: total)
        }
        return nil
    }
}
