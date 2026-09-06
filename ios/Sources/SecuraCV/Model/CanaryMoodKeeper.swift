// CanaryMoodKeeper.swift
//
// The phone's copy of the living canary's slow feelings: persists the
// CanaryMoodState (anxiety decays over HOURS, trust builds over DAYS — it
// must survive relaunches or the character has amnesia), guards the
// engine's once-per-minute tick against our faster refresh cadence, rolls
// the trust ladder at local midnight, and folds the fleet's current truth
// into the engine's inputs. Pure over injected defaults + clock;
// host-tested. The engine itself is the shared mirror of the display
// firmware's bird_mood.h — one character, same feelings, every surface.
//
// "Every surface" now includes the Apple TV: the Witness Wall compiles this
// exact file (tvos/WitnessWall/project.yml lists it, the CloudContainer
// pattern), so the wall's bird has the same slow feelings and the same
// amnesia guards as the phone's. That is why this file knows nothing about
// the phone's Witness rows — the fleet fold lives in
// CanaryMoodInputs+Fleet.swift, which stays iPhone-only.

import Foundation

struct CanaryMoodKeeper {
    static let stateKey = "canary_mood_state_v1"
    static let lastTickKey = "canary_mood_last_tick_v1"
    static let lastDayKey = "canary_mood_last_day_v1"

    private let defaults: UserDefaults
    private let calendar: Calendar

    init(defaults: UserDefaults = .standard, calendar: Calendar = .current) {
        self.defaults = defaults
        self.calendar = calendar
    }

    /// The persisted slow state (fresh bird on first run).
    private(set) var state: CanaryMoodState {
        get {
            guard let data = defaults.data(forKey: Self.stateKey),
                  let s = try? JSONDecoder().decode(CanaryMoodState.self, from: data) else {
                return CanaryMoodState()
            }
            return s
        }
        nonmutating set {
            if let data = try? JSONEncoder().encode(newValue) {
                defaults.set(data, forKey: Self.stateKey)
            }
        }
    }

    /// Fold fresh inputs into the mood. Call as often as you like — the
    /// engine's minute tick fires at most once per minute (its decay math
    /// counts minutes, so a 20-second caller must not triple time), while a
    /// worsened floor is adopted IMMEDIATELY (trouble never waits for the
    /// clock; that's the engine's own "rises instantly" rule). Returns the
    /// current face/posture and whether a trust milestone was crossed today.
    @discardableResult
    func observe(_ inputs: CanaryMoodInputs, now: Date = Date())
        -> (face: CanaryFace, posture: CanaryPosture, state: CanaryMoodState, milestone: Bool) {
        var s = state
        var milestone = false

        // Local-day rollover first. Exactly ONE elapsed day is the normal
        // midnight: roll the ladder (and maybe cross a milestone). Two or
        // more means the keeper wasn't watching — and unobserved days are
        // unknown, not clean. "Consecutive clean days" means consecutively
        // PROVEN, so a gap resets the streak instead of awarding trust for
        // days nobody witnessed.
        let today = calendar.startOfDay(for: now)
        if let lastDay = defaults.object(forKey: Self.lastDayKey) as? Date {
            let from = calendar.startOfDay(for: lastDay)
            let elapsed = calendar.dateComponents([.day], from: from, to: today).day ?? 0
            if elapsed == 1 {
                let previous = s.trustDays
                CanaryMoodEngine.rollover(&s)
                if CanaryMoodEngine.trustMilestone(previousDays: previous, days: s.trustDays) {
                    milestone = true
                }
            } else if elapsed > 1 {
                s.trustDays = 0
                s.dayClean = true
            }
        }
        defaults.set(today, forKey: Self.lastDayKey)

        // The minute tick, minute-guarded…
        let lastTick = defaults.object(forKey: Self.lastTickKey) as? Date ?? .distantPast
        if now.timeIntervalSince(lastTick) >= 60 {
            CanaryMoodEngine.minute(&s, inputs)
            defaults.set(now, forKey: Self.lastTickKey)
        } else {
            // …but a WORSE floor lands immediately, mid-minute (and marks
            // the day dirty exactly as the engine's tick would).
            let floorNow = CanaryMoodEngine.anxietyFloor(inputs)
            if floorNow > s.anxiety {
                s.anxiety = floorNow
                s.calmMinutes = 0
            }
            if floorNow > 0 || inputs.alarmUnacked { s.dayClean = false }
        }

        state = s
        let face = CanaryMoodEngine.face(s, inputs)
        return (face, CanaryMoodEngine.posture(face, inputs), s, milestone)
    }
}

