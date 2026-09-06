// ProximityRanger.swift  (SHARED — pure Foundation, host-tested)
//
// The arithmetic behind "Find this Canary": turn the presence beacon's noisy
// RSSI stream into something a human can walk toward. Every Canary already
// broadcasts continuously (Wire/FleetBeacon), and an unfiltered scan with
// duplicates on hears it several times a second — which is exactly the
// signal a warmer/colder search needs.
//
// What this deliberately is NOT: a distance meter. RSSI is not meters — a
// wall, a hand, a pocket all move it by more than a step does — and the
// ESP32 has no ultra-wideband radio, so there is no honest arrow to draw.
// So the ranger speaks in BANDS with hedged words ("about a room away"),
// smooths hard, and refuses to flicker:
//
//   * EMA smoothing eats the per-advert jitter.
//   * Hysteresis: a band is easy to enter while the signal climbs and takes
//     a real drop to leave, so the label never strobes at a boundary.
//   * Staleness: a beacon not heard for a few seconds is "listening…",
//     never a stale "right here" — the same honesty rule the fleet rows
//     follow (BLEConsole.staleAfter).
//   * The TREND (warmer/colder) is the real navigation aid — it is what a
//     human actually uses to walk a hot/cold search.
//
// The haptic grammar for a finding session lives here too (FindingTick),
// pure and host-tested, in the FeedbackPolicy tradition: a buzz per BAND
// crossed on the way in, one success when you arrive, and silence while
// nothing changes — never a tick per advert.

import Foundation

/// How close the beacon says you are, in honest bands. Ordered so `>` means
/// closer.
enum ProximityBand: Int, Comparable, Hashable, Sendable, CaseIterable {
    case searching = 0    // no fresh beacon — listening
    case faint            // heard, at the edge
    case far              // somewhere around here
    case near             // about a room away
    case veryClose        // a few steps
    case here             // within arm's reach

    static func < (a: ProximityBand, b: ProximityBand) -> Bool { a.rawValue < b.rawValue }

    /// The big word on the screen.
    var label: String {
        switch self {
        case .searching: return "Listening…"
        case .faint: return "Faint signal"
        case .far: return "In range"
        case .near: return "Getting close"
        case .veryClose: return "Very close"
        case .here: return "Right here"
        }
    }

    /// The hedged sentence under it — words, never fake meters.
    var hint: String {
        switch self {
        case .searching: return "No beacon heard yet — walk around; walls and doors matter."
        case .faint: return "At the edge of hearing. Keep moving and watch for Warmer."
        case .far: return "Somewhere around here — roughly a couple of rooms."
        case .near: return "About a room away."
        case .veryClose: return "A few steps. Look up and down too — shelves count."
        case .here: return "Within arm's reach, roughly."
        }
    }

    /// How full the finding ring draws (0…1). Searching keeps a sliver so
    /// the screen never looks dead while it listens.
    var ringFraction: Double {
        switch self {
        case .searching: return 0.06
        case .faint: return 0.2
        case .far: return 0.4
        case .near: return 0.6
        case .veryClose: return 0.8
        case .here: return 1.0
        }
    }

    /// Smoothed-RSSI floor (dBm) to ENTER each band while approaching.
    /// Typical ESP32 advertising power; a band is a word, not a ruler.
    var entryDBM: Double? {
        switch self {
        case .searching: return nil
        case .faint: return -95
        case .far: return -85
        case .near: return -73
        case .veryClose: return -61
        case .here: return -49
        }
    }
}

/// Which way the search is going — the actual navigation aid.
enum ProximityTrend: Hashable, Sendable {
    case warmer
    case colder
    case steady
    case unknown     // not enough history yet

    var label: String? {
        switch self {
        case .warmer: return "Warmer"
        case .colder: return "Colder"
        case .steady, .unknown: return nil
        }
    }
}

/// One finding session's haptic grammar — pure, so the discipline is a test,
/// not a hope. Ticks fire on BAND TRANSITIONS only: hysteresis upstream
/// guarantees a transition is a real change, so an ordinary still moment
/// produces nothing (FeedbackPolicy's rule, applied to the one screen where
/// the user explicitly asked to be guided by the hand).
enum FindingTick: Hashable, Sendable {
    case closer(ProximityBand)   // crossed inward — graded by how close
    case arrived                 // reached "right here" — the success tap
    case lost                    // a close signal vanished — one gentle note
}

struct ProximityRanger: Sendable {
    /// A beacon quiet this long is not evidence of presence.
    static let staleAfter: TimeInterval = 6
    /// EMA weight per sample — heavy smoothing on purpose; adverts arrive
    /// several times a second, so convergence is still quick.
    static let smoothing = 0.3
    /// How far the smoothed signal must fall below a band's entry to LEAVE
    /// it downward — the anti-flicker margin.
    static let demoteHysteresisDB = 5.0
    /// Trend looks this far back, needs at least `trendMinSpan` of history,
    /// and calls a move only past `trendThresholdDB`.
    static let trendWindow: TimeInterval = 5
    static let trendMinSpan: TimeInterval = 2
    static let trendThresholdDB = 2.0

    private(set) var smoothedDBM: Double?
    private(set) var lastHeard: Date?
    private(set) var band: ProximityBand = .searching
    private var history: [(at: Date, dbm: Double)] = []

    struct Reading: Hashable, Sendable {
        var band: ProximityBand
        var trend: ProximityTrend
        var smoothedDBM: Int?
    }

    /// Feed one advert. Returns the post-sample reading.
    mutating func ingest(rssiDBM: Int, at now: Date = Date()) -> Reading {
        let sample = Double(rssiDBM)
        let ema = smoothedDBM.map { $0 + Self.smoothing * (sample - $0) } ?? sample
        smoothedDBM = ema
        lastHeard = now
        history.append((now, ema))
        history.removeAll { now.timeIntervalSince($0.at) > Self.trendWindow }
        band = Self.nextBand(current: band, smoothed: ema)
        return Reading(band: band, trend: trend(now: now), smoothedDBM: Int(ema.rounded()))
    }

    /// Re-evaluate without a sample — the staleness check the UI ticks on.
    mutating func reading(at now: Date = Date()) -> Reading {
        if let last = lastHeard, now.timeIntervalSince(last) > Self.staleAfter {
            band = .searching
            smoothedDBM = nil
            history.removeAll()
        }
        return Reading(band: band, trend: trend(now: now),
                       smoothedDBM: smoothedDBM.map { Int($0.rounded()) })
    }

    /// Band selection with hysteresis: promote the moment the smoothed
    /// signal clears a floor; demote only once it falls the margin below.
    static func nextBand(current: ProximityBand, smoothed: Double) -> ProximityBand {
        let promoted = ProximityBand.allCases.last {
            guard let floor = $0.entryDBM else { return true }   // searching always qualifies
            return smoothed >= floor
        } ?? .searching
        if promoted > current { return promoted }
        let demoted = ProximityBand.allCases.last {
            guard let floor = $0.entryDBM else { return true }
            return smoothed >= floor - demoteHysteresisDB
        } ?? .searching
        if demoted < current { return demoted }
        return current
    }

    private func trend(now: Date) -> ProximityTrend {
        guard let newest = history.last,
              let oldest = history.first,
              newest.at.timeIntervalSince(oldest.at) >= Self.trendMinSpan else { return .unknown }
        let delta = newest.dbm - oldest.dbm
        if delta >= Self.trendThresholdDB { return .warmer }
        if delta <= -Self.trendThresholdDB { return .colder }
        return .steady
    }

    // MARK: - the haptic grammar

    /// What (if anything) a band transition deserves in the hand.
    static func tick(from old: ProximityBand, to new: ProximityBand) -> FindingTick? {
        guard new != old else { return nil }
        if new == .here { return .arrived }
        if new > old, new >= .far { return .closer(new) }
        // A signal that vanished from close range gets ONE gentle note —
        // the user was almost there and the screen alone might be pocketed.
        if new == .searching, old >= .near { return .lost }
        return nil
    }

    // MARK: - the twin rule

    /// Two fleet members sharing the beacon's 2-byte suffix cannot be told
    /// apart over the air — FleetMerge.attach's ambiguity rule, stated once
    /// here so the phone's Find screen and the wrist's apply the SAME rule
    /// (a search that ignored it could walk the user to the WRONG Canary
    /// while saying "Right here"). `fingerprints` is every fleet member's
    /// fingerprint, the target's included.
    static func isSuffixAmbiguous(fingerprint: String, among fingerprints: [String]) -> Bool {
        guard fingerprint.count >= 4 else { return false }
        let suffix = fingerprint.lowercased().suffix(4)
        return fingerprints.filter {
            $0.count >= 4 && $0.lowercased().hasSuffix(suffix)
        }.count > 1
    }

    // MARK: - the multi-Canary hint

    /// "You're closer to the Kitchen one right now." Given the target's
    /// smoothed level and the other Canaries currently heard, name the
    /// neighbor that clearly outranks the target — or nil when nothing
    /// does. The margin keeps ordinary noise from renaming the winner.
    static func nearerNeighbor(targetDBM: Double?,
                               neighbors: [(name: String, dbm: Double)],
                               marginDB: Double = 8) -> String? {
        guard let best = neighbors.max(by: { $0.dbm < $1.dbm }) else { return nil }
        guard let target = targetDBM else { return best.name }
        return best.dbm >= target + marginDB ? best.name : nil
    }
}
