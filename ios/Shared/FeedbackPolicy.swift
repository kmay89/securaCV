// FeedbackPolicy.swift  (SHARED — the one arbiter of when the app may buzz)
//
// Haptics and sound are the fastest way to make a security app unbearable,
// so every buzz on every surface goes through THIS pure policy, and the
// policy knows only transitions and deliberate actions — never polling
// churn. The discipline (the §5b doctrine, made mechanical):
//
//   * A refresh that changes nothing produces NOTHING. Ever.
//   * Escalation is felt once, at the moment the fleet's worst severity
//     crosses INTO alert-or-worse — not on every cycle it stays there.
//   * The all-clear is felt once, when the fleet drops back below alert —
//     closure, not chatter. Notices and warns are silent on their own:
//     an ordinary week produces zero haptics.
//   * A path self-test the USER started answers in the hand that asked.
//
// Pure Foundation, host-tested; the per-platform adapters (iOS haptics +
// chirp, watch WKInterfaceDevice taps) only translate events to hardware.

import Foundation

enum FeedbackEvent: Equatable, Sendable {
    /// The fleet's worst severity crossed into alert (firm) or tamper
    /// (firmest). Carries the new severity so adapters can grade intensity.
    case fleetEscalated(to: Severity)
    /// Below alert again after alert-or-worse — the gentle "it's over".
    case allClear
    /// The end-to-end alert-path self-test verified — the canary chirps.
    case pathVerified
    case pathFailed
}

enum FeedbackPolicy {
    /// Decide what (if anything) a fleet-severity transition deserves.
    /// The all-clear fires on LEAVING alarm territory (alert-or-worse →
    /// anything below it), so a fleet that settles through "needs a look"
    /// still delivers its closure. (No demo-mode guard needed: the demo
    /// fleet never fakes an alarm — a tested invariant — so anything that
    /// escalates is real.)
    static func fleetTransition(from old: Severity, to new: Severity) -> FeedbackEvent? {
        if new > old && new >= .alert { return .fleetEscalated(to: new) }
        if old >= .alert && new < .alert { return .allClear }
        return nil
    }

    /// Decide what a finished path self-test deserves. Only called for
    /// user-started tests — an automatic background beat stays silent.
    static func pathTest(verified: Bool) -> FeedbackEvent {
        verified ? .pathVerified : .pathFailed
    }
}
