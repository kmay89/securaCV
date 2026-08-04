// GlanceAnswer.swift  (SHARED — the one spoken sentence, for every surface
// that ASKS instead of looks)
//
// Siri, Shortcuts, Spotlight, the Action button — anywhere the user asks
// "is everything okay?" without opening the app — get their answer composed
// here, from the same WristSnapshot every glance surface renders. One
// composer, host-tested, so the spoken answer can never tell a different
// story than the widgets and the wrist.
//
// The honesty rules travel with the words:
//   * An old snapshot is still answered, but the sentence SAYS its age — an
//     "all's well" from an hour ago presented as current is exactly the
//     false comfort this app exists to refuse.
//   * A quiet fleet with a dark delivery path answers with both facts; green
//     rows do not outrank a heartbeat that stopped.
//   * Sample data is labeled in the sentence itself, same as the banner.
//   * Pure Foundation. No AppIntents, no SwiftUI — the intents render these
//     strings; they never invent their own.

import Foundation

enum GlanceAnswer {
    /// A snapshot older than this still answers, prefixed with its age.
    /// Matches the practical staleness of the glance cache: the app rewrites
    /// it on every real change while it runs; a phone that hasn't run the app
    /// since this morning should say so.
    static let staleAfter: TimeInterval = 15 * 60

    /// The whole answer to "how is the fleet?", from the cached glance.
    static func spoken(_ snapshot: WristSnapshot?, now: Date = Date()) -> String {
        guard let snap = snapshot else {
            return "SecuraCV hasn't met your fleet yet — open the app once and this will answer."
        }
        var sentences: [String] = []
        let counts = "\(snap.healthy) of \(snap.total) healthy"
        if snap.severity == .ok {
            sentences.append("All's well — \(counts).")
        } else {
            sentences.append("\(spokenHeadline(snap.headline)) — \(counts).")
        }
        // The dead-man's-switch outranks green rows: a fleet that LOOKS fine
        // while the provably-alive path is dark or failed must say both.
        if snap.severity == .ok,
           snap.heartbeat == .dark || snap.heartbeat == .failed {
            sentences.append(ensurePeriod(snap.heartbeatSummary(now: now)))
        }
        if snap.isDemoData { sentences.append("Sample data.") }
        let answer = sentences.joined(separator: " ")
        if let age = agePhrase(since: snap.sentAt, now: now) {
            return "As of \(age): \(answer)"
        }
        return answer
    }

    /// The Quiet Hour confirmation. Always names the punch-through, so the
    /// user hears — every single time — what a mute can never silence.
    static func quieted(count: Int) -> String {
        guard count > 0 else {
            return "Nothing to quiet — no Canaries are paired yet."
        }
        let who = count == 1 ? "your Canary" : "all \(count) Canaries"
        return "Quieted \(who) for the next hour. Tamper and signature failures still come through."
    }

    /// The symmetric verb's confirmation — honest when there was nothing to do.
    static func resumed(count: Int) -> String {
        guard count > 0 else {
            return "Nothing was muted — alerts were already at full volume."
        }
        return "Alerts are back to full volume."
    }

    /// The path test's verdict, spoken. `summary` is HeartbeatCopy's sentence
    /// (the same one the provably-alive card and the wrist show) — wrapped
    /// when it verified, passed through untouched when it did not, because a
    /// failure's wording is the system's real objection, not ours to soften.
    static func pathTest(verified: Bool, summary: String) -> String {
        guard verified else { return ensurePeriod(summary) }
        return "Your fleet can reach you — \(lowercasedFirst(summary))."
    }

    // MARK: - composition helpers

    /// The rollup headline is typeset for glass ("Front Porch • Activity");
    /// a spoken sentence reads the separator as a pause, not a glyph.
    private static func spokenHeadline(_ headline: String) -> String {
        headline.replacingOccurrences(of: " • ", with: ": ")
    }

    /// nil while fresh; a human phrase once the snapshot is old enough that
    /// presenting it as current would mislead.
    private static func agePhrase(since: Date, now: Date) -> String? {
        let age = now.timeIntervalSince(since)
        guard age > staleAfter else { return nil }
        let minutes = Int(age / 60)
        if minutes < 120 { return "\(minutes) minutes ago" }
        let hours = minutes / 60
        if hours < 48 { return "about \(hours) hours ago" }
        return "\(hours / 24) days ago"
    }

    private static func ensurePeriod(_ s: String) -> String {
        s.hasSuffix(".") ? s : s + "."
    }

    private static func lowercasedFirst(_ s: String) -> String {
        guard let first = s.first else { return s }
        return first.lowercased() + s.dropFirst()
    }
}
