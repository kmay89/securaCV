// CanaryVoice.swift
//
// The bird helper — the website's header-bird assistant, ported to the phone.
// The origin is the site's js/canary-herald.js: one speech bubble hanging off
// the mascot, one channel that serves double duty — the Canary talking to you
// (a hello, an orientation line, a milestone) and the app talking to you (a
// notice worth a sentence). Same bird, same bubble, different tone. This file
// ports the origin's pure helpers constant-for-constant (the pacing scale,
// the dwell curve, message normalization) and lifts the ordering rules out of
// its DOM-layer closures into a pure queue — over there those rules are
// pinned by source-matching tests; here they run headlessly.
// CanaryVoiceTests pins each behavior the website's tests pin.
//
// What deliberately did NOT come along:
//   * The website mascot's own feelings vocabulary (happy/celebrate/…) and
//     its per-message gestures. On Apple surfaces the character's face comes
//     from ONE place — the mood engine mirrored in Shared/CanaryMood.swift
//     (bird_mood.h is the source of truth), and every motion maps 1:1 to
//     state a log line can name. A tone here colors the bubble and paces the
//     words; it never paints an expression on the bird.
//   * URL actions. The website refuses unsafe hrefs at runtime (safeHref —
//     "none of them should be able to turn the header into a redirector");
//     here the same rule is structural: an action can only name an AppSection,
//     so there is nothing to sanitize and nowhere external to go.
//   * The word "Herald". That name belongs to the e-paper placard concept
//     (docs/hardware/canary_herald_research.md); to users the assistant was
//     always just "your Canary", and that is the register this file keeps.
//   * The site's offline/online announcements — its only self-initiated
//     warn. A static page keeps working offline, which is worth saying out
//     loud; the app's link truth already lives in the mood engine, worn on
//     the face and spoken by the moodLine, and the voice does not repeat
//     what the character is already showing.

import Foundation

/// How a line means, not just what it says — the website's TONES list.
///   chat  the Canary talking (hello, a nudge); no accent color
///   info  an orientation line
///   good  good news
///   warn  a notice that waits to be acknowledged
/// The site accepts unknown tone strings and falls back to chat; the enum
/// makes that rule structural.
enum CanaryVoiceTone: String, Equatable, Sendable {
    case chat, info, good, warn
}

/// Where a message may send you: an in-app section, nothing else. The label
/// is capped and trimmed by `compose`, mirroring the site's 32-char rule.
struct CanaryVoiceAction: Equatable, Sendable {
    var label: String
    var section: AppSection
}

/// One normalized message, safe to show. Build these through
/// `CanaryVoiceEngine.compose` — never by hand — so the caps and defaults
/// hold everywhere.
struct CanaryVoiceMessage: Equatable, Sendable {
    var text: String            // trimmed, ≤ 180 characters
    var tone: CanaryVoiceTone
    var ttl: TimeInterval       // 0 = stays until acknowledged
    var action: CanaryVoiceAction?
    var key: String?            // dedupe: the same key never queues twice
    var chatter: Bool           // volunteered small talk — mutable to quiet
}

/// One pacing scale for everything the bird says — the site's PACE table,
/// milliseconds turned into seconds, values identical. Message timing is its
/// own concern, separate from the character's motion timing (CanaryActor's
/// loop periods) — this is about reading, that is about moving.
enum CanaryVoicePace {
    static let base: TimeInterval = 2.2      // time to notice a bubble at all
    static let perChar: TimeInterval = 0.048 // ≈250 words/min — a glance, not a book
    static let action: TimeInterval = 1.5    // something to tap needs a beat longer
    static let floor: TimeInterval = 4.2     // never so brief that looking away loses it
    static let ceiling: TimeInterval = 14.0  // never so long that it becomes furniture
    static let gap: TimeInterval = 0.26      // between one message leaving and the next
    static let hello: TimeInterval = 1.4     // the first-launch introduction, after things settle
    static let tip: TimeInterval = 2.6       // a section's orientation line, once it's been seen
    // The site's `regrant` (re-arm after a pointer leaves the bubble) has no
    // finger equivalent; it is consciously not ported rather than left dead.
}

enum CanaryVoiceEngine {
    static let maxQueue = 4            // a backlog past this is noise, not news
    static let dismissalsToQuiet = 3   // brush off this many and the chatter stops
    static let maxTextLength = 180
    static let maxLabelLength = 32
    static let maxTTL: TimeInterval = 30

    /// How long a message should stay up: long enough to read, and not a
    /// second longer. The clamp ORDER is load-bearing and test-pinned, same
    /// as the site: the reading time hits the floor/ceiling first, THEN the
    /// action beat is added, then one final cap — the other way around lets
    /// the floor swallow the bonus, so "Saved." with a button to tap got no
    /// more time than "Saved." without one.
    static func dwell(for text: String, hasAction: Bool) -> TimeInterval {
        let n = text.trimmingCharacters(in: .whitespacesAndNewlines).count
        if n == 0 { return 0 }
        let read = Swift.max(CanaryVoicePace.floor,
                             Swift.min(CanaryVoicePace.ceiling,
                                       CanaryVoicePace.base + Double(n) * CanaryVoicePace.perChar))
        return Swift.min(CanaryVoicePace.ceiling,
                         read + (hasAction ? CanaryVoicePace.action : 0))
    }

    /// Take whatever a caller passed and return a message the bird can
    /// trust, or nil if there's nothing worth saying — the site's
    /// normalizeMessage. A runaway ttl is clamped; a warning defaults to
    /// staying until acknowledged (ttl 0) because a warning you missed did
    /// no work; an action with an empty label is dropped whole rather than
    /// shown blank.
    static func compose(_ text: String,
                        tone: CanaryVoiceTone = .chat,
                        ttl: TimeInterval? = nil,
                        action: CanaryVoiceAction? = nil,
                        key: String? = nil,
                        chatter: Bool = false) -> CanaryVoiceMessage? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        var kept: CanaryVoiceAction?
        if let action {
            let label = action.label.trimmingCharacters(in: .whitespacesAndNewlines)
            if !label.isEmpty {
                kept = CanaryVoiceAction(label: String(label.prefix(maxLabelLength)),
                                         section: action.section)
            }
        }

        let life: TimeInterval
        if let ttl, ttl.isFinite {   // a non-finite ttl is "no ttl given", as in the origin
            life = Swift.max(0, Swift.min(maxTTL, ttl))
        } else {
            life = tone == .warn ? 0 : dwell(for: trimmed, hasAction: kept != nil)
        }

        return CanaryVoiceMessage(text: String(trimmed.prefix(maxTextLength)),
                                  tone: tone,
                                  ttl: life,
                                  action: kept,
                                  // An empty key is no key — the origin nullifies
                                  // it so nothing ever dedupes against "".
                                  key: (key?.isEmpty == false) ? key : nil,
                                  chatter: chatter)
    }
}

/// The one line of messages, with the site's ordering rules intact. Pure —
/// the stage (CanaryVoiceStage) owns the clocks; this owns the truth about
/// who speaks next.
struct CanaryVoiceQueue: Equatable {
    private(set) var showing: CanaryVoiceMessage?
    private(set) var waiting: [CanaryVoiceMessage] = []

    var hasWaiting: Bool { !waiting.isEmpty }

    enum Verdict: Equatable {
        case refused        // quiet, or a duplicate key — nothing changed
        case queued         // accepted; present when the bubble is free
        case interrupted    // a warning cut the small talk that was showing
    }

    /// The site's say(): dedupe by key against both the showing message and
    /// the line; a warning does not take its turn — it goes to the FRONT,
    /// and if the bubble is busy with small talk it takes the slot outright
    /// (the chatter is dropped, never requeued: it was never important
    /// enough to come back for). Overflow trims from the BACK, never the
    /// front — the front is where the warning is.
    mutating func offer(_ message: CanaryVoiceMessage, quiet: Bool) -> Verdict {
        if message.chatter && quiet { return .refused }
        let duplicate = { (m: CanaryVoiceMessage?) -> Bool in
            guard let m, let a = m.key, let b = message.key else { return false }
            return a == b
        }
        if duplicate(showing) || waiting.contains(where: { duplicate($0) }) { return .refused }

        if message.tone == .warn {
            waiting.insert(message, at: 0)
            if let s = showing, s.chatter {
                // The interrupt path returns before the trim, exactly as the
                // origin's early return does — the cut chatter already paid
                // for the warning's seat.
                showing = nil
                return .interrupted
            }
        } else {
            waiting.append(message)
        }
        if waiting.count > CanaryVoiceEngine.maxQueue {
            waiting.removeLast(waiting.count - CanaryVoiceEngine.maxQueue)
        }
        return .queued
    }

    /// Move the next message on stage, if the stage is empty.
    mutating func advance() -> CanaryVoiceMessage? {
        guard showing == nil, !waiting.isEmpty else { return nil }
        showing = waiting.removeFirst()
        return showing
    }

    /// The showing message leaves (timed out, dismissed, or acted on).
    mutating func hide() {
        showing = nil
    }

    /// Put a message back at the front of the line — used when an alarm
    /// seizes the stage mid-sentence, so a real notice waits for the alarm
    /// to end instead of dwelling away invisibly.
    mutating func restage(_ message: CanaryVoiceMessage) {
        showing = nil
        waiting.insert(message, at: 0)
    }
}
