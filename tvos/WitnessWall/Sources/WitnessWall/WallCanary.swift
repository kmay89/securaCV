//  WallCanary.swift — this wall's truth, in the mood engine's terms.
//
//  The Wall compiles the same character the phone and the watch stage
//  (CanaryMood + CanaryActor + CanaryMoodKeeper, listed from ios/ in
//  project.yml) — one mood engine, many renderers. What each surface owns
//  is the FOLD from its truth to the engine's inputs, and the honesty rule
//  travels with it: every field maps to state a log line on THIS surface
//  can name, and a field this surface cannot name stays at its honest zero
//  rather than being invented.
//
//  What the Wall can and cannot say:
//    lostWitnesses  — devices reporting offline. The fleet endpoint has no
//                     per-device grace ladder, so the Wall knows dark, not
//                     late; staleWitnesses stays 0 and the bird's worried
//                     posture is always Calling, never Searching.
//    linksDown      — a device that cannot reach its hub, or a wall that
//                     lost its own source (the stale state).
//    allVerified    — never claimed on this surface, yet. The repo reserves
//                     "verified" for an Ed25519 signature checked against a
//                     key PINNED at pairing — nothing looser — and today
//                     this TV walks a served log against the key the log
//                     itself supplied. A passing walk is real evidence and
//                     the header phrases it carefully ("not yet pinned");
//                     the engine's full-verified snap (anxiety to zero at
//                     once) is a stronger claim than that, so the bird
//                     earns calm the slow way. The day pairing pins a key,
//                     this is the field that lights up.
//    alarmUnacked   — the Wall's real alarm: a chain that did not verify
//                     (this TV's own verdict, or a device saying so).
//                     There is no acknowledgment on a wall, so the alarm
//                     holds the stage until the state clears — the bird is
//                     .hidden whenever that alarm is live, including while
//                     a stale wall remembers it under the warning banner.
//    hubFlapping, unackedOld, night — no source on this surface; honest
//                     zeros. tvOS has no quiet-hours concept and the Wall
//                     never sleeps on purpose (the idle timer is disabled).
//
//  The one sentence mirrors the phone's composeMoodLine word for word where
//  the states match — same character, same voice — and stays ambient: it
//  may rephrase contentment or name who is being called for, never word an
//  alarm (faces .hidden and .asleep return nil; the banners own trouble).

import Foundation

enum WallCanary {
    /// The fold. `wallDown` is the degrade path: the wall itself lost every
    /// source, which is a link problem of this TV's own, worth exactly the
    /// engine's linksDown weight and nothing invented on top.
    static func inputs(fleet: FleetSnapshot,
                       wallDown: Bool,
                       report: VerifyReport?) -> CanaryMoodInputs {
        var i = CanaryMoodInputs()
        i.lostWitnesses = fleet.devices.filter { !$0.online }.count
        i.linksDown = wallDown || fleet.devices.contains { $0.hubState == .down }
        // Never claimed yet: the engine's full-verified snap is reserved
        // for a chain walked against a key pinned at pairing ("verified"
        // means nothing looser — AGENTS.md), and today's walk checks the
        // key the log itself supplied. A FAILED walk is still a real alarm
        // below — the same asymmetry the header banner speaks.
        i.allVerified = false
        i.alarmUnacked = report?.ok == false || fleet.hasChainTrouble
        return i
    }

    /// The one ambient sentence beside the bird — the Voice rule: every
    /// line names log-able state, and an alarm is never worded (the banner
    /// slot owns it; .hidden and .asleep return nil).
    static func line(face: CanaryFace,
                     posture: CanaryPosture,
                     state: CanaryMoodState,
                     milestone: Bool,
                     fleet: FleetSnapshot) -> String? {
        switch face {
        case .hidden, .asleep:
            return nil   // the instruments own the stage
        case .calm:
            if milestone {
                return state.trustDays >= 30 ? "A clean month together"
                                             : "A clean week together"
            }
            if state.trustDays >= 7 {
                return "\(state.trustDays) clean days together"
            }
            return "Watching with you"
        case .worried:
            switch posture {
            case .calling:
                if let lost = fleet.devices.first(where: { !$0.online }) {
                    return "Calling for \(lost.name)"
                }
                return "Calling for a lost Canary"
            case .searching:
                // Unreachable on this surface (staleWitnesses is always 0),
                // kept for totality: the engine owns the ladder, not us.
                return "Looking for a quiet Canary…"
            case .asFace:
                return "Something feels off"
            }
        case .distressed:
            return "Feeling rough — the fleet needs care"
        }
    }
}
