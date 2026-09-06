// CanaryMoodInputs+Fleet.swift
//
// The iPhone's fold from fleet truth to the mood engine's terms. This lived
// in CanaryMoodKeeper.swift until the Apple TV started compiling the keeper
// (the Wall shares the file so the two birds keep one set of slow feelings);
// the fold references the phone's Witness rows, which the Wall does not
// have, so it moved here and stays iPhone-only. The Wall's own fold is
// tvos/WitnessWall/Sources/WitnessWall/WallCanary.swift — same honesty
// rule, that surface's truth.

import Foundation

extension CanaryMoodInputs {
    /// The fleet's current truth, in the engine's terms. Every field maps
    /// to state a log line can name — the honesty rule, upheld at the fold.
    /// The caller decides `alarmUnacked` from PRE-mute severity
    /// (`displaySeverity`): a muted alarm is still a live alarm, and the
    /// bird stays hidden until someone actually acknowledges it.
    @MainActor
    init(fleet: [Witness], alarmUnacked: Bool) {
        self.init()
        staleWitnesses = fleet.filter { $0.link == .stale }.count
        lostWitnesses = fleet.filter { $0.link.isDark }.count
        allVerified = !fleet.isEmpty && fleet.allSatisfy {
            $0.link == .online && $0.badge == .verified && !$0.tamper
        }
        // A live Alert/Tamper nobody acknowledged hands the stage to the
        // instruments (face == .hidden — never cute during a real alarm).
        self.alarmUnacked = alarmUnacked
    }
}
