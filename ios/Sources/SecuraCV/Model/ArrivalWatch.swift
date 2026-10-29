// ArrivalWatch.swift
//
// "Watch it appear" used to tick a Canary as yours only when THIS phone had
// joined its setup network (the network's four identity characters match
// the device's mDNS host). Every other way in ended in an open-ended
// "Watching…" or a list of every same-kind Canary in the house: the Bluetooth
// card, the iPhone Camera app scanning the glass QR, the Flasher with Wi-Fi
// typed in. Nobody was told which one was new.
//
// This is the other half: what was ALREADY on the network when the person
// opened the walkthrough is the baseline, and a same-kind Canary heard after
// that is new. The screen says so in those words ("new since you opened
// this"), never "verified" or "paired": it is a strong hint, not an identity
// proof, and the identity match above still wins whenever it exists.
//
// The trap is a late browse. When Local Network access is still being asked
// for, or the browse restarts after the app was in the background (the
// Camera-app route does exactly that), devices that were always there arrive
// late and would look new. So the baseline collects for `settle` seconds
// counted from the first moment the browse is actually RUNNING while the
// walkthrough is open, never from the moment the screen appeared.

import Foundation

struct ArrivalWatch: Equatable, Sendable {
    /// How long, once the browse runs, everything heard counts as already
    /// here. mDNS answers from a quiet network land well inside this.
    static let settle: TimeInterval = 10

    private(set) var baseline: Set<String> = []
    /// When the settle window began: the first observation with the browse
    /// running. Nil until then.
    private(set) var settleFrom: Date?

    /// Fold one observation and return the ids that arrived after the
    /// baseline settled, in the order given. Call it whenever the browse
    /// state or the heard list changes.
    mutating func observe(browsing: Bool, ids: [String], at now: Date) -> [String] {
        guard browsing else {
            // Nothing heard while the browse is down counts either way; the
            // baseline already collected is kept for when it comes back.
            return []
        }
        let from = settleFrom ?? now
        settleFrom = from
        if now.timeIntervalSince(from) < Self.settle {
            baseline.formUnion(ids)
            return []
        }
        return ids.filter { !baseline.contains($0) }
    }

    /// Whether the baseline has settled — before that, nothing is "new".
    func settled(at now: Date) -> Bool {
        guard let from = settleFrom else { return false }
        return now.timeIntervalSince(from) >= Self.settle
    }
}
