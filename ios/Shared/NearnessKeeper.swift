// NearnessKeeper.swift  (SHARED — pure Foundation, host-tested)
//
// The ambient half of finding: which Canary is this phone NEAREST right
// now? The active search (ProximityRanger) answers "am I getting closer to
// the one I picked?"; this answers the question nobody has to ask — the
// fleet just knows, from the same presence beacons it already hears.
//
// An ambient claim has stricter calm rules than an active one, because
// nobody asked for it and nobody is walking anywhere:
//
//   * THE LIST NEVER RE-SORTS. Nearness is a whisper on a row (a small
//     glyph), never an ordering — severity owns the order, everywhere,
//     always. This type only ever names ONE id; the rendering stays a
//     badge by construction.
//   * AN INCUMBENT KEEPS THE TITLE until a challenger BEATS it — by a real
//     margin (8 dB, the same noise bar the finding hint uses) AND for a
//     sustained dwell (two consecutive evaluations). Ordinary RSSI churn
//     between two rooms must never make the badge hop.
//   * A QUIET BEACON LOSES THE TITLE. No fresh advert inside the window →
//     that Canary can't be "nearest"; no fresh advert from anyone → nobody
//     is, and the badge simply isn't drawn. Absence of the claim is the
//     honest default, exactly like the finding screen's "Listening…".
//
// The caller (FleetStore) feeds it matched sightings at the fold cadence
// and excludes demo rows — sample data must never wear a real-radio badge.

import Foundation

struct NearnessKeeper: Sendable {
    /// A beacon older than this can't back a nearness claim — the same
    /// honesty window the active ranger uses.
    static let staleAfter: TimeInterval = ProximityRanger.staleAfter
    /// How decisively a challenger must outrank the incumbent — the same
    /// noise bar as the finding screen's "closer to X" hint.
    static let marginDB: Double = 8
    /// A challenger must hold its lead this many CONSECUTIVE evaluations
    /// before taking the title — one strong advert is weather, two folds of
    /// sustained lead is a move.
    static let dwellEvaluations = 2
    /// Per-id smoothing weight; the fold cadence is slow (seconds), so a
    /// heavier step than the active ranger's converges usefully.
    static let smoothing = 0.4

    private var smoothed: [String: (dbm: Double, at: Date)] = [:]
    private(set) var nearestID: String?
    private var challengerID: String?
    private var challengerStreak = 0

    /// Feed one witness's freshest sighting (already matched by the caller).
    mutating func observe(id: String, rssiDBM: Int, at now: Date = Date()) {
        let sample = Double(rssiDBM)
        let ema = smoothed[id].map { $0.dbm + Self.smoothing * (sample - $0.dbm) } ?? sample
        smoothed[id] = (ema, now)
    }

    /// Re-pick after a round of observations. Returns the (possibly nil)
    /// holder; the caller publishes it.
    @discardableResult
    mutating func evaluate(at now: Date = Date()) -> String? {
        let cutoff = now.addingTimeInterval(-Self.staleAfter)
        smoothed = smoothed.filter { $0.value.at >= cutoff }

        // A quiet incumbent abdicates before anything else is decided.
        if let holder = nearestID, smoothed[holder] == nil {
            nearestID = nil
        }
        guard let best = smoothed.max(by: { $0.value.dbm < $1.value.dbm }) else {
            nearestID = nil
            challengerID = nil
            challengerStreak = 0
            return nil
        }
        // No holder → the strongest fresh voice takes the title outright;
        // there is no incumbent to protect.
        guard let holder = nearestID, let holderDBM = smoothed[holder]?.dbm else {
            nearestID = best.key
            challengerID = nil
            challengerStreak = 0
            return nearestID
        }
        // The incumbent keeps the title unless the SAME challenger leads by
        // the margin for the full dwell.
        if best.key != holder, best.value.dbm >= holderDBM + Self.marginDB {
            if challengerID == best.key {
                challengerStreak += 1
            } else {
                challengerID = best.key
                challengerStreak = 1
            }
            if challengerStreak >= Self.dwellEvaluations {
                nearestID = best.key
                challengerID = nil
                challengerStreak = 0
            }
        } else {
            challengerID = nil
            challengerStreak = 0
        }
        return nearestID
    }
}
