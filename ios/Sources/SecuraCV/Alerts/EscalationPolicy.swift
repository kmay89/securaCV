// EscalationPolicy.swift
//
// "Nobody answered." The ops world's oldest good idea: an alarm that goes
// unacknowledged gets louder, and eventually reaches somebody else. The thing
// that makes it work — the part consumer apps skip — is RATIONING. Escalate
// everything and the second alert is just spam with extra steps; escalate
// only the top of the ladder and the second buzz keeps meaning "this one is
// different" (docs/design/iphone_companion_app.md §5b rule 5).
//
// So this policy is deliberately narrow:
//   * TOP TIER ONLY — tamper/panic, and a signature that did not verify.
//     Nothing else escalates, ever, no matter how long it sits.
//   * ONCE per occurrence. A repeat of the condition reopens the lifecycle
//     (the ledger clears the stamp), but one live alarm cannot buzz twice.
//   * An acknowledgment anywhere — phone, wrist, notification action — ends
//     it. That is the whole point of having an ack.
//
// The second-household-member leg is NOT here: that needs the relay
// (docs/design/alert_relay.md R1). What this file can do today is re-alert
// the user's own devices, which is the leg we actually have.

import Foundation

enum EscalationPolicy {
    /// How long an unanswered top-tier alarm waits before it says so again.
    /// Short enough to matter during an incident, long enough that a user
    /// walking back to their phone answers the first one.
    static let delay: TimeInterval = 300

    /// Every timestamp this policy can read is a 10-minute bucket floored
    /// DOWN (Invariant III), so "how long ago" read straight off a record
    /// over-reports by up to one bucket — and escalation would fire early,
    /// occasionally instantly. Paying the full bucket back makes the wait
    /// conservative: never sooner than `delay`, never later than
    /// `delay + bucketWidth`. Honest copy for that is "still unacknowledged",
    /// never a claimed number of minutes.
    static let bucketWidth: TimeInterval = 600

    /// The rationing rule, in one place. Tamper is the top of the severity
    /// ladder; a failed signature sits at `.alert` but is the same class of
    /// news (someone or something is interfering with the witness), so it
    /// escalates too. Nothing else does.
    static func isTopTier(severity: Severity, integrityFailed: Bool) -> Bool {
        severity >= .tamper || integrityFailed
    }

    static func shouldEscalate(severity: Severity,
                               integrityFailed: Bool,
                               firstPosted: Date,
                               now: Date,
                               acknowledged: Bool,
                               alreadyEscalated: Bool) -> Bool {
        guard isTopTier(severity: severity, integrityFailed: integrityFailed) else { return false }
        guard !acknowledged, !alreadyEscalated else { return false }
        return now.timeIntervalSince(firstPosted) >= delay + bucketWidth
    }

    /// The second notification's words. It must be obviously the SAME alarm
    /// rather than a new one — a user who reads "Tamper detected" twice has
    /// been told about two break-ins, which is a lie the first alert didn't
    /// tell.
    static func body(name: String, statusLine: String) -> String {
        "Still unacknowledged — \(name): \(statusLine)"
    }
}
