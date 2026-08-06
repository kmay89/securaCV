// WitnessAlertPrefs.swift
//
// Per-witness reach: "the porch Canary can tell me anything; the garage one
// only when it's serious." Rules are global-by-severity, which is the right
// default and the wrong only option — one busy doorway shouldn't force the
// whole fleet down to its noise level, and turning a rule off to quiet one
// device silences the other five.
//
// The ladder deliberately has NO "never". You can narrow what a Canary is
// allowed to interrupt you about; you cannot switch it off. A per-witness
// "never" would be an untimed mute with a nicer name — the exact silence
// this product may not ship (mute is temporary and punches through for
// tamper; this floor is permanent, so its bottom rung must still carry
// tamper). Someone who truly wants nothing from a device can unpair it, and
// that is an honest, visible act.
//
// Durable across the witness rebuild for the same reason MuteLedger is: rows
// are re-derived from device truth every refresh, so anything the user chose
// has to live outside them.

import Foundation

/// How much a single Canary is allowed to interrupt you.
enum WitnessPushFloor: Int, Codable, CaseIterable, Sendable, Identifiable {
    case armed = 0        // whatever the rules arm — the default
    case serious = 1      // alarms and worse
    case tamperOnly = 2   // only tamper/panic gets through

    var id: Int { rawValue }

    /// Nothing below this severity may push for this witness.
    var minSeverity: Severity {
        switch self {
        case .armed: return .ok
        case .serious: return .alert
        case .tamperOnly: return .tamper
        }
    }

    var title: String {
        switch self {
        case .armed: return "Everything I've armed"
        case .serious: return "Only serious things"
        case .tamperOnly: return "Only tamper"
        }
    }

    var explanation: String {
        switch self {
        case .armed:
            return "This Canary can reach you for anything your rules arm."
        case .serious:
            return "Alarms and lost-device alerts still reach you; everyday activity stays in the app."
        case .tamperOnly:
            return "Only tamper or panic reaches you. Everything else is still recorded and still shown here — it just won't buzz."
        }
    }

    /// What the Alerts tab shows on an alert this floor held back — the
    /// user's own choice, quoted back so the row is never a mystery.
    var undeliveredReason: String {
        switch self {
        case .armed: return "No armed rule covers this."
        case .serious: return "You asked this Canary for serious things only."
        case .tamperOnly: return "You asked this Canary for tamper only."
        }
    }
}

struct WitnessAlertPrefs {
    static let key = "witness_push_floor_v1"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func floor(for id: String) -> WitnessPushFloor {
        guard let raw = table[id], let floor = WitnessPushFloor(rawValue: raw) else { return .armed }
        return floor
    }

    func set(_ floor: WitnessPushFloor, for id: String) {
        var t = table
        if floor == .armed {
            t.removeValue(forKey: id)      // the default is stored as absence
        } else {
            t[id] = floor.rawValue
        }
        save(t)
    }

    /// Witnesses the user has narrowed — the rules sheet says how many, so a
    /// per-device choice made months ago can't become an invisible reason
    /// alerts aren't arriving.
    func narrowedIDs() -> [String] {
        table.keys.sorted()
    }

    // MARK: - storage ({id: floor raw})

    private var table: [String: Int] {
        (defaults.dictionary(forKey: Self.key) as? [String: Int]) ?? [:]
    }

    private func save(_ t: [String: Int]) {
        defaults.set(t, forKey: Self.key)
    }
}
