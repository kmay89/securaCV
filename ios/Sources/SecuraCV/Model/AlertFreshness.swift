// AlertFreshness.swift
//
// The two surfaces that turn the alert lifecycle into something the user
// actually experiences (docs/design/alerts_event_history.md §6.1 and §6.3).
// Both are pure functions over the ledger the app already keeps — no new
// state, nothing on the wire, nothing to drift.
//
//  1. THE ALL-CLEAR. When a condition ends, the Alerts row flips to
//     "Cleared" — but only for someone already looking at that tab. The
//     status-page lesson is that silence must never be the only "it's fine"
//     signal, so the *transition* earns a line in Today: "Front Porch —
//     Gone dark, now clear". Digest tier by construction: it is derived, not
//     posted, so there is no code path by which an all-clear could buzz.
//
//  2. "WHILE YOU WERE AWAY". Every other camera app's recovery story is
//     "hope the user scrolls". Ours can be explicit, because the ledger is
//     durable state and notifications are only its projection: unseen rows
//     ARE the diff since the user last looked, so one honest sentence can
//     lead the tab — how much happened, and how much of it is still live.

import Foundation

enum AlertFreshness {
    /// How far back an all-clear is worth showing. Today is a day's story;
    /// a clear from last week belongs to the Alerts tab's history, not here.
    static let allClearWindow: TimeInterval = 86_400

    /// The resolutions of the last day, as timeline entries. Derived from the
    /// ledger every fold, so a record that gets removed or swept takes its
    /// all-clear with it — there is no second copy to go stale.
    static func allClearEvents(_ records: [AlertRecord],
                               within window: TimeInterval = allClearWindow,
                               now: Date = Date()) -> [TimelineEvent] {
        let cutoff = now.addingTimeInterval(-window)
        return records.compactMap { record in
            guard let resolved = record.resolvedBucket, resolved >= cutoff else { return nil }
            return TimelineEvent(id: "allclear#\(record.id)",
                                 deviceID: record.witnessID,
                                 deviceName: record.name,
                                 zone: "",
                                 headline: allClearHeadline(for: record),
                                 severity: .ok,
                                 // NOT a signed device claim — this line is
                                 // the phone's own observation that an alarm
                                 // ended, and it wears the unverified badge
                                 // so it can never be mistaken for one of the
                                 // fleet's witnessed records.
                                 badge: .unknown,
                                 timeBucket: resolved,
                                 symbol: "checkmark.circle")
        }
    }

    /// Names what ended, in the words the alert itself used. Deliberately not
    /// a guessed recovery verb ("back online" is wrong for a tamper that
    /// stopped): we know exactly one true thing — this condition is over —
    /// so that is what the line says.
    static func allClearHeadline(for record: AlertRecord) -> String {
        "\(record.name) — \(record.headline), now clear"
    }

    /// The one line that leads the Alerts tab after time away. nil when there
    /// is nothing new to reconcile, which is most of the time — this is a
    /// recovery surface, not a permanent banner.
    static func awaySummary(_ records: [AlertRecord]) -> String? {
        let unseen = records.filter(\.isUnseen)
        guard !unseen.isEmpty else { return nil }
        let needs = unseen.filter(\.needsYou).count
        let happened = unseen.count == 1 ? "1 thing happened" : "\(unseen.count) things happened"
        switch needs {
        case 0: return "\(happened) while you were away — all of it is over now."
        case 1: return "\(happened) while you were away — 1 still needs you."
        default: return "\(happened) while you were away — \(needs) still need you."
        }
    }
}
