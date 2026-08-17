// RepeatGovernor.swift
//
// The dog-at-the-door problem, solved as policy rather than as a mute.
//
// The condition ledger in FleetStore already guarantees "one alert per
// condition" — but a condition is a severity + a status line, and a Canary
// watching a dog pace the porch flips between two or three status lines all
// evening. Every flip is a NEW condition to the ledger, so every flip buzzed.
// Nobody turns the volume down on a smoke alarm; everybody turns it down on
// a doorbell that won't stop. This file is the difference.
//
// The shape borrows from how people actually habituate: the FIRST alert of a
// burst deserves a full interruption; repeats carry almost no new
// information, so each one earns a longer rest than the last (120 s doubling
// to a 30-minute ceiling). A calm half hour ends the burst — the next alert
// is a fresh story and buzzes like one. Two things always pierce, no matter
// how tired the burst is:
//
//   * a REAL escalation — the repeat is more severe than anything already
//     buzzed in this burst (activity became an alarm);
//   * tamper or a failed signature — the smoke-alarm tier is never governed,
//     the same guarantee Witness.effectiveSeverity makes for mutes.
//
// Rested repeats are not dropped: the ledger still collapses them into the
// record (count, lastBucket), so the history stays complete — they just stop
// reaching the pocket. Pure and host-tested; FleetStore applies it.

import Foundation

enum RepeatGovernor {
    /// First rest after a burst's opening buzz.
    static let baseCooldown: TimeInterval = 120
    /// Rests double per repeat up to this ceiling.
    static let cooldownCap: TimeInterval = 1800
    /// Quiet this long ends the burst — the next alert is news again.
    static let calmGap: TimeInterval = 1800

    /// What one witness's burst looks like so far. Kept by FleetStore per
    /// witness id, in memory: a relaunch forgetting a burst costs at most one
    /// extra buzz, which is the honest direction to fail.
    struct Memory: Hashable, Sendable {
        var lastBuzzAt: Date
        /// The worst severity already buzzed in this burst — the bar a
        /// repeat must clear to count as an escalation.
        var worstBuzzed: Severity
        /// Buzzes so far in this burst; drives the doubling.
        var buzzCount: Int
    }

    /// The decision for one would-be repeat.
    struct Verdict: Hashable, Sendable {
        var buzz: Bool
        var memory: Memory
        /// When `buzz` is false: how much longer this burst rests.
        var restingFor: TimeInterval?
    }

    /// The rest a burst has earned after `buzzCount` buzzes.
    static func cooldown(afterBuzzes buzzCount: Int) -> TimeInterval {
        guard buzzCount > 0 else { return 0 }
        let doubled = baseCooldown * pow(2, Double(buzzCount - 1))
        return min(cooldownCap, doubled)
    }

    /// Should this (changed) condition interrupt, given the burst so far?
    ///
    /// `tamper`/`integrityFailed` are passed separately from severity so the
    /// punch-through cannot be weakened by a severity mapping change: the
    /// smoke-alarm tier pierces because of WHAT it is, not how it ranks.
    static func consider(severity: Severity,
                         tamper: Bool,
                         integrityFailed: Bool,
                         memory: Memory?,
                         now: Date) -> Verdict {
        // The smoke-alarm tier is never governed.
        if tamper || integrityFailed {
            let count = (memory?.buzzCount ?? 0) + 1
            let worst = max(memory?.worstBuzzed ?? severity, severity)
            return Verdict(buzz: true,
                           memory: Memory(lastBuzzAt: now, worstBuzzed: worst,
                                          buzzCount: count),
                           restingFor: nil)
        }
        // No burst, or the last one ended calmly — a fresh story.
        guard let memory, now.timeIntervalSince(memory.lastBuzzAt) < calmGap else {
            return Verdict(buzz: true,
                           memory: Memory(lastBuzzAt: now, worstBuzzed: severity,
                                          buzzCount: 1),
                           restingFor: nil)
        }
        // A genuine escalation pierces the rest.
        if severity > memory.worstBuzzed {
            return Verdict(buzz: true,
                           memory: Memory(lastBuzzAt: now, worstBuzzed: severity,
                                          buzzCount: memory.buzzCount + 1),
                           restingFor: nil)
        }
        // A same-or-milder repeat inside the burst: buzz only once the
        // earned rest has passed.
        let rest = cooldown(afterBuzzes: memory.buzzCount)
        let elapsed = now.timeIntervalSince(memory.lastBuzzAt)
        if elapsed >= rest {
            return Verdict(buzz: true,
                           memory: Memory(lastBuzzAt: now,
                                          worstBuzzed: max(memory.worstBuzzed, severity),
                                          buzzCount: memory.buzzCount + 1),
                           restingFor: nil)
        }
        return Verdict(buzz: false, memory: memory, restingFor: rest - elapsed)
    }

    /// The ledger's honest sentence for a rested repeat — the history must
    /// say WHY this one didn't reach the pocket, in the user's terms.
    static func restingReason(_ restingFor: TimeInterval) -> String {
        let minutes = max(1, Int((restingFor / 60).rounded(.up)))
        return "Grouped with the last alert from this Canary — repeats rest for \(minutes) min."
    }
}

/// Cross-witness storm collapse: when several Canaries cross into alert in
/// the same evaluation pass — the router died, the power blinked — the
/// news is ONE event ("the house lost its watchers"), not N phones-worth
/// of separate buzzes. Below the threshold, individual alerts read better
/// (each names its Canary); at or past it, a single summary reads better
/// and the ledger still keeps every per-Canary record.
enum AlertStorm {
    /// Three simultaneous alerts is where separate buzzes stop informing
    /// and start punishing.
    static let threshold = 3

    /// The one notification thread a storm summary posts under.
    static let threadID = "fleet-storm"

    struct Pending: Hashable, Sendable {
        var name: String
        var severity: Severity
        var statusLine: String
    }

    struct Summary: Hashable, Sendable {
        var body: String
        var worst: Severity
    }

    /// Nil below the threshold — post individually. At or past it, one
    /// summary naming the count and the worst thing happening.
    static func collapse(_ pending: [Pending]) -> Summary? {
        guard pending.count >= threshold else { return nil }
        guard let worst = pending.max(by: { $0.severity < $1.severity }) else { return nil }
        let body = "\(pending.count) Canaries need attention. Worst: \(worst.name) — \(worst.statusLine)"
        return Summary(body: body, worst: worst.severity)
    }
}
