// MuteLedger.swift
//
// The durable half of per-witness mute. Witness rows are REBUILT from device
// truth on every refresh, so a mute stored on the row alone would evaporate
// in 20 seconds; this ledger persists {witness id → muted-until} and is
// re-applied at every fold. Muting caps nagging at Notice but never hides
// tamper or a failed signature — that punch-through lives in
// Witness.effectiveSeverity, so no path through this ledger can weaken it.
// Pure over injected UserDefaults; host-tested.

import Foundation

struct MuteLedger {
    static let key = "witness_mutes_v1"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func muteUntil(for id: String) -> Date? {
        guard let until = table[id], until > Date() else { return nil }
        return Date(timeIntervalSince1970: until.timeIntervalSince1970)
    }

    func set(until: Date, for id: String) {
        var t = table
        t[id] = until
        save(t)
    }

    func clear(_ id: String) {
        var t = table
        t.removeValue(forKey: id)
        save(t)
    }

    /// Ids whose mute is still in force — the Resume Alerts verb's honest
    /// count ("nothing was muted" must be distinguishable from "cleared 5").
    func activeMutes(now: Date = Date()) -> [String] {
        table.filter { $0.value > now }.map(\.key)
    }

    /// Back to full volume in one verb (Resume Alerts, from Siri/Shortcuts).
    func clearAll() {
        save([:])
    }

    /// Stamp active mutes onto freshly folded rows and prune expired entries
    /// (a ledger that only grows is a slow leak).
    func apply(to witnesses: inout [Witness], now: Date = Date()) {
        var t = table
        var dirty = false
        for (id, until) in t where until <= now {
            t.removeValue(forKey: id)
            dirty = true
        }
        if dirty { save(t) }
        guard !t.isEmpty else { return }
        for i in witnesses.indices {
            if let until = t[witnesses[i].id] {
                witnesses[i].mutedUntil = until
            }
        }
    }

    // MARK: - storage ({id: secondsSince1970})

    private var table: [String: Date] {
        guard let raw = defaults.dictionary(forKey: Self.key) as? [String: Double] else { return [:] }
        return raw.mapValues { Date(timeIntervalSince1970: $0) }
    }

    private func save(_ t: [String: Date]) {
        defaults.set(t.mapValues { $0.timeIntervalSince1970 }, forKey: Self.key)
    }
}
