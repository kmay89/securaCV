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

    // Named apart from the dictionaries they read: a method sharing a stored
    // property's base name is a coin-flip at the call site even where the
    // compiler allows it.
    func actedCount(_ severity: Severity) -> Int { acted[String(severity.rawValue)] ?? 0 }
    func dismissedCount(_ severity: Severity) -> Int { dismissed[String(severity.rawValue)] ?? 0 }
    func total(_ severity: Severity) -> Int { actedCount(severity) + dismissedCount(severity) }

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

    /// "Keep pushing these" — remembered by class key, so the offer is made
    /// once and never becomes its own nag.
    func decline(key: String) {
        var d = declined
        d.insert(key)
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
        /// EVERY rule that would push this class — not just the strongest.
        /// The shipped rules overlap by severity (both "Signature / chain
        /// broke" and "A Canary went dark" cover an alarm), so turning off
        /// one leaves the other pushing the very class the button just
        /// promised to stop. An offer that doesn't do what it says is worse
        /// than no offer.
        let ruleIDs: [String]
        let ruleTitles: [String]
        let severity: Severity
        let dismissed: Int
        let total: Int

        /// Keyed by CLASS, not by rule: "keep pushing these" is an answer
        /// about a kind of alert, and it must stick even though the set of
        /// rules covering that kind can change.
        var id: String { "sev-\(severity.rawValue)" }

        var className: String { ruleTitles.joined(separator: " / ") }

        var sentence: String {
            "You've dismissed \(dismissed) of the last \(total) “\(className)” alerts."
        }

        var question: String {
            let tail = "They'll still be recorded, and still show up here — they just won't interrupt you."
            guard ruleTitles.count > 1 else { return "Stop pushing these? \(tail)" }
            return "Stop pushing these? All \(ruleTitles.count) rules that cover them switch off. \(tail)"
        }
    }

    /// The one offer worth making right now, if any. Only for classes that
    /// actually push — advising someone to turn off something already silent
    /// is how "smart" features lose trust, and everyday activity is silent by
    /// construction (digest events never push, so they never get here).
    static func advice(stats: AlertActionStats,
                       rules: [AlertRule],
                       declined: Set<String>) -> Advice? {
        // Least-serious first: if two classes qualify, the everyday noise is
        // what gets offered for demotion — nobody should be asked to stop
        // pushing tamper while a lesser class is the thing they actually
        // dismiss all day.
        for severity in Severity.allCases.sorted(by: <) {
            // The smoke alarm is not tunable. Tamper is never offered for
            // demotion however often it gets dismissed — and silencing it
            // would take the classes below with it anyway (every rule that
            // covers an alarm also covers a tamper), which is more than
            // anyone asked for.
            guard severity < .tamper else { break }
            let total = stats.total(severity)
            guard total >= minimumSample else { continue }
            let dismissed = stats.dismissedCount(severity)
            guard Double(dismissed) / Double(total) >= dismissThreshold else { continue }
            let pushing = AlertRule.pushing(for: severity, in: rules)
            guard !pushing.isEmpty else { continue }
            let advice = Advice(ruleIDs: pushing.map(\.id),
                                ruleTitles: pushing.map(\.title),
                                severity: severity,
                                dismissed: dismissed, total: total)
            guard !declined.contains(advice.id) else { continue }
            return advice
        }
        return nil
    }
}
