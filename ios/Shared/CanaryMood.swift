// CanaryMood.swift  (SHARED — the living canary's feelings, ported faithfully)
//
// A Swift mirror of the display firmware's mood engine
// (firmware/projects/canary-display/.../bird_mood.h — THE source of truth;
// change that header first, then this file, keeping every constant equal).
// The glass on the nightstand and the watch on the wrist must be the SAME
// character with the same feelings about the same fleet — one mood engine,
// many renderers. That is what makes the character a story instead of a
// sticker.
//
// The honesty rule travels with it (the Pwnagotchi property): every face
// maps 1:1 to state a log line can name. No random sadness for variety, no
// cheerful mask over a degraded system. And the alarm rule above all:
// Hidden — never cute during a real alarm; the character hands the stage to
// the instruments.
//
// Two slow scalars, pointed at SYSTEM health:
//   anxiety 0..14 — rises instantly with real trouble, decays one point per
//                   fully-quiet hour, snaps to zero on a fully-verified pass.
//   trust (days)  — consecutive fully-clean days; a long-healthy system is
//                   VISIBLY different from a day-one system.

// SecuraCV-Parity: every Apple surface that shows a device compiles this.
// (the one mood engine behind every bird)

import Foundation

/// Mirror of `canary::care::BirdInputs`.
struct CanaryMoodInputs: Equatable, Sendable {
    var staleWitnesses: Int = 0     // amber: late past grace
    var lostWitnesses: Int = 0      // red: officially missing
    var linksDown: Bool = false     // wifi or hub link currently down
    var hubFlapping: Bool = false   // link dropped more than once this hour
    var unackedOld: Int = 0         // Warn+ unacknowledged > 12 h
    var allVerified: Bool = false   // every witness fresh AND chain-verified
    var night: Bool = false         // quiet hours (bedside semantics; the
                                    // phone passes false until it learns them)
    var alarmUnacked: Bool = false  // live Alert/Tamper, not acknowledged
}

/// Mirror of `canary::care::BirdMood` — the persisted slow state.
struct CanaryMoodState: Codable, Equatable, Sendable {
    var anxiety: Int = 0            // 0..14
    var trustDays: Int = 0          // consecutive clean days
    var dayClean: Bool = true       // no trouble seen since the last rollover
    var calmMinutes: Int = 0        // quiet minutes toward the hourly decay
}

/// The face ladder — mirror of `canary::care::BirdFace`, same raw values.
/// Escalation is by silhouette (calm-tech): the serious end hands the stage
/// to the instrument UI entirely.
enum CanaryFace: UInt8, Codable, CaseIterable, Sendable {
    case hidden = 0      // alarm handoff — never cute during a real alarm
    case asleep          // night: stillness IS the information
    case calm            // all quiet; the idle pool plays
    case worried         // anxiety 4..9 — something is late, the bird shows it
    case distressed      // anxiety 10..14 — visibly unwell

    init(tolerant raw: Int) { self = CanaryFace(rawValue: UInt8(clamping: raw)) ?? .calm }
}

/// Posture refinement inside the Worried band — mirror of
/// `canary::care::BirdPosture`. The CAUSE picks the story: Searching for the
/// late, Calling for the lost (lost outranks late).
enum CanaryPosture: UInt8, Codable, Sendable {
    case asFace = 0
    case searching
    case calling

    init(tolerant raw: Int) { self = CanaryPosture(rawValue: UInt8(clamping: raw)) ?? .asFace }
}

enum CanaryMoodEngine {
    static let anxietyMax = 14

    /// Mirror of `bird_anxiety_floor` — identical weights, identical cap.
    static func anxietyFloor(_ i: CanaryMoodInputs) -> Int {
        let a = 2 * i.staleWitnesses + 4 * i.lostWitnesses +
            (i.hubFlapping ? 3 : 0) + (i.linksDown ? 2 : 0) +
            1 * i.unackedOld
        return min(a, anxietyMax)
    }

    /// Mirror of `bird_mood_minute` — the once-per-minute tick. Anxiety
    /// rises instantly to the floor, decays one point per fully-quiet hour
    /// above it, and a verified pass clears it. Trouble of any kind marks
    /// the day dirty for the trust ladder.
    static func minute(_ m: inout CanaryMoodState, _ i: CanaryMoodInputs) {
        let floorA = anxietyFloor(i)
        if floorA > m.anxiety {
            m.anxiety = floorA
            m.calmMinutes = 0
        } else if i.allVerified && floorA == 0 {
            m.anxiety = 0    // the full-pass snap: everything answered and proved
            m.calmMinutes = 0
        } else if m.anxiety > floorA {
            m.calmMinutes += 1
            if m.calmMinutes >= 60 {
                m.calmMinutes = 0
                m.anxiety -= 1
            }
        } else {
            m.calmMinutes = 0
        }
        if floorA > 0 || i.alarmUnacked { m.dayClean = false }
    }

    /// Mirror of `bird_mood_rollover` — local-day rollover: a clean day
    /// earns a trust day; a dirty one starts the streak over.
    static func rollover(_ m: inout CanaryMoodState) {
        if m.dayClean {
            if m.trustDays < 60000 { m.trustDays += 1 }
        } else {
            m.trustDays = 0
        }
        m.dayClean = true
    }

    /// Mirror of `bird_face` — the ladder, same bands, same precedence.
    static func face(_ m: CanaryMoodState, _ i: CanaryMoodInputs) -> CanaryFace {
        if i.alarmUnacked { return .hidden }
        if i.night { return .asleep }
        if m.anxiety >= 10 { return .distressed }
        if m.anxiety >= 4 { return .worried }
        return .calm
    }

    /// Mirror of `bird_posture` — lost outranks late; link trouble stays
    /// plain Worried (there is nobody specific to look for).
    static func posture(_ f: CanaryFace, _ i: CanaryMoodInputs) -> CanaryPosture {
        guard f == .worried else { return .asFace }
        if i.lostWitnesses > 0 { return .calling }
        if i.staleWitnesses > 0 { return .searching }
        return .asFace
    }

    /// Mirror of `bird_trust_milestone` — true only on the exact crossing
    /// of the first clean week / first clean month.
    static func trustMilestone(previousDays: Int, days: Int) -> Bool {
        (previousDays < 7 && days >= 7) || (previousDays < 30 && days >= 30)
    }
}
