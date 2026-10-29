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
// late and would look new. So the baseline collects for `settle` seconds of
// UNINTERRUPTED running browse while the walkthrough is open. A stretch that
// is cut short (the browse stops, or the app leaves the foreground, which
// stops it) restarts the window on the next run, and everything already
// collected is kept. Review on the first cut: a window that kept counting
// through the background let an owned display that answered on return look
// new.

import Foundation

struct ArrivalWatch: Equatable, Sendable {
    /// How long, of uninterrupted running browse, everything heard counts
    /// as already here. mDNS answers from a quiet network land well inside.
    static let settle: TimeInterval = 10

    private(set) var baseline: Set<String> = []
    /// Start of the current uninterrupted running stretch, while unsettled.
    private(set) var windowStart: Date?
    private(set) var isSettled = false

    /// The browse stopped or the app left the foreground: an unsettled
    /// window starts over on the next run. What was collected is kept.
    mutating func interrupt() {
        if !isSettled { windowStart = nil }
    }

    /// Fold one observation and return the ids that arrived after the
    /// baseline settled, in the order given. Call it whenever the browse
    /// state or the heard list changes.
    mutating func observe(browsing: Bool, ids: [String], at now: Date) -> [String] {
        guard browsing else {
            interrupt()
            return []
        }
        if !isSettled {
            let start = windowStart ?? now
            windowStart = start
            if now.timeIntervalSince(start) < Self.settle {
                baseline.formUnion(ids)
                return []
            }
            isSettled = true
        }
        return ids.filter { !baseline.contains($0) }
    }

    /// Whether the baseline has settled — before that, nothing is "new".
    func settled(at now: Date) -> Bool {
        if isSettled { return true }
        guard let start = windowStart else { return false }
        return now.timeIntervalSince(start) >= Self.settle
    }
}
