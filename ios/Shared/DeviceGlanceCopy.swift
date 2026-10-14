// DeviceGlanceCopy.swift — hand-written.
//
// The short sentences a GLANCE says about one device: how its room stands
// (the radar's coarse wellbeing words) and how it stands with its hub. The
// Witness Wall's card and the watch's row and detail screen both say these,
// from a distance and in one line — so they are written once, here, and
// both surfaces compile them. Before this file the Wall owned the only copy
// (WallView.swift `wallWellbeingLine`) and the wrist said nothing at all.
//
// The phone's detail screen keeps its own labeled rows (Presence /
// Occupants / Breathing rhythm) — a list, not a line — but in the same
// words: "present"/"clear", a count that tops out at 2+, a breathing rhythm
// "sensed"/"not sensed", never a vital-signs claim.
//
// Pure Foundation. Absence is never rendered as calm: every function returns
// nil when the device said nothing, and the caller then draws NOTHING.

// SecuraCV-Parity: every Apple surface that shows a device compiles this.
// (the one-line wellbeing and hub sentences)

import Foundation

enum DeviceGlanceCopy {
    /// The coarse wellbeing line — the radar's room story and, on surfaces
    /// that carry it, the camera's seeing claim — folded to one quiet
    /// sentence. Nil when none of the inputs is present.
    ///
    /// `seeing` is the raw wire word; only the four the fleet vocabulary
    /// holds (person / vehicle / animal / package — Invariant II ends the
    /// list) render, and anything else is silence, never a guess.
    static func wellbeingLine(present: Bool?, occupants: Int?, breathing: Bool?,
                              seeing: String? = nil, seeingScore: Int? = nil) -> String? {
        var parts: [String] = []
        if let present {
            parts.append(present ? "someone present" : "room clear")
        }
        if let occupants {
            // The radar's contract is 0 / 1 / 2-meaning-2-or-more — it
            // deliberately cannot count a crowd, so neither may this label
            // (the phone's Witness.occupantsLabel rule).
            parts.append(occupants >= 2 ? "2+ in the room" : "\(occupants) in the room")
        }
        if let breathing {
            // "Sensed", never a vital-signs claim — the lock is a rhythm the
            // radar can currently hold, nothing more.
            parts.append(breathing ? "breathing rhythm sensed" : "breathing rhythm not sensed")
        }
        if let seen = seeingPhrase(seeing) {
            parts.append(seeingScore.map { "\(seen) · \($0)%" } ?? seen)
        }
        guard let first = parts.first else { return nil }
        parts[0] = first.prefix(1).uppercased() + String(first.dropFirst())
        return parts.joined(separator: " · ")
    }

    /// The seeing word folded to a phrase, for exactly the four words the
    /// fleet vocabulary holds.
    static func seeingPhrase(_ word: String?) -> String? {
        switch word {
        case "person": return "seeing a person"
        case "vehicle": return "seeing a vehicle"
        case "animal": return "seeing an animal"
        case "package": return "seeing a package"
        default: return nil
        }
    }

    /// One quiet line about the hub, only when there is something to say: a
    /// Canary with no hub is EXPLAINED (the fleet runs without one), never
    /// raised as an error. Nil for a connected hub and for a device that
    /// never said.
    static func hubLine(_ hub: HubState) -> String? {
        switch hub {
        case .absent: return "No hub yet — it works on its own"
        case .down: return "Can't reach its hub"
        case .ok, .unknown: return nil
        }
    }
}
