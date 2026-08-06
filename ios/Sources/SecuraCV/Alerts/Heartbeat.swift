// Heartbeat.swift
//
// "Provably alive" — the smoke-alarm chirp, but for the whole delivery path.
// This tracks the two different things that can be true about liveness, keeps
// them apart, and never lets the weaker one wear the stronger one's words:
//
//   * PATH VERIFIED (`lastVerified`) — an alert actually reached this phone
//     and iOS accepted it. Proven by the one-tap Test Alert, and by any real
//     alert that got through. This is the claim "we can reach you".
//   * FLEET CHECK-IN (`lastBeat` with a `.fleetCheckIn` source) — a Canary
//     answered. Proof the fleet is up and talking to this app; NOT proof that
//     a notification would land. It says "your fleet checked in", and the
//     copy says exactly that (HeartbeatCopy) rather than borrowing the word
//     "verified" for something that wasn't.
//
// It is ALSO the dead-man's-switch: if the FLEET's beats stop past the dark
// window, the SILENCE is itself the alarm (docs §5b). Three guards keep that
// honest — two against crying wolf (a false dead-man's-switch is how a real
// one gets ignored) and one against the opposite, staying green through a
// real outage:
//   0. ONLY THE FLEET'S SILENCE COUNTS. The dark verdict reads
//      `lastFleetCheckIn`, never `lastVerified` — otherwise the notification
//      *about* a Canary going dark would refresh the timer that was supposed
//      to be running out, and the switch would sit green while the fleet was
//      gone.
//   1. SILENCE ONLY COUNTS WHILE WE WERE LISTENING. The app hears nothing in
//      the background by design (the radios are stopped), so time spent
//      backgrounded is not evidence of anything. `noteListening()` restarts
//      the window at every foreground.
//   2. SILENCE ONLY COUNTS IF SOMETHING WAS EXPECTED. With nothing paired
//      there is no beat to miss, so `expectsBeats` stays false and the card
//      says "not yet verified" instead of raising an alarm about a fleet the
//      user hasn't got yet.
//
// `lastVerified`/`lastBeat` persist: an app relaunch must not throw away a
// verification the user actually earned (before this, every cold start reset
// the card to "Not yet verified" and the Test Alert had to be re-run to say
// anything at all).

import Foundation

@MainActor
final class Heartbeat: ObservableObject {
    enum PathState: Equatable {
        case unknown
        case alive(secondsAgo: Int)
        case testing
        case dark(sinceSeconds: Int)     // missed too many beats — this is an alert
        case failed(String)

        var isHealthy: Bool { if case .alive = self { return true }; return false }
    }

    @Published private(set) var state: PathState = .unknown
    /// Last end-to-end proof that an alert can reach this phone.
    @Published private(set) var lastVerified: Date?
    /// Last signal of any kind — a verification, or the fleet checking in.
    /// This is what the card DISPLAYS.
    @Published private(set) var lastBeat: Date?
    /// What that last signal was, so the card can't overstate it.
    @Published private(set) var lastBeatSource: WristBeatSource?
    /// Last time the FLEET answered — and the only input to the dark verdict.
    /// Kept apart from `lastBeat` on purpose: the dead-man's-switch asks "has
    /// the fleet gone silent?", and a notification we managed to deliver is
    /// not an answer to that question. Sharing one timestamp meant the alert
    /// *about* a Canary going dark refreshed the very timer that was supposed
    /// to be running out — the switch would sit green while the fleet was
    /// gone, which is the one failure it exists to prevent.
    @Published private(set) var lastFleetCheckIn: Date?

    /// Beats are expected on this cadence; miss `missTolerance` in a row → dark.
    var beatInterval: TimeInterval = 300      // 5 min
    var missTolerance: Int = 3

    /// Is there anything whose silence would MEAN something? Set by
    /// FleetStore from the paired fleet — no devices, no dead-man's-switch.
    var expectsBeats = false

    /// Since when this app has actually been able to hear a beat. Nil until
    /// the first foreground; silence before that proves nothing.
    private var listeningSince: Date?

    static let verifiedKey = "heartbeat_last_verified_v1"
    static let beatKey = "heartbeat_last_beat_v1"
    static let beatSourceKey = "heartbeat_last_beat_source_v1"

    private let defaults: UserDefaults

    init(defaults: UserDefaults? = nil) {
        self.defaults = defaults
            ?? UserDefaults(suiteName: PhoneGlanceCache.appGroupID)
            ?? .standard
        load()
    }

    /// The fleet answers every refresh (20s) and every sentinel pass (5s).
    /// Stamping each one would move the published state — and therefore the
    /// wrist snapshot and the widget timelines — constantly, for no new
    /// information. One beat a minute is far inside the dark window and is
    /// all any surface renders.
    static let beatCoalescing: TimeInterval = 60

    /// The wire's copy of `lastBeat`, floored to 5 minutes. A snapshot whose
    /// bytes moved on every refresh would wake the watch and the widgets on a
    /// 20-second cadence to say the same thing; and flooring DOWN can only
    /// make a beat look OLDER than it is, which is the honest direction for a
    /// liveness claim to round.
    var wireLastBeat: Date? {
        lastBeat.map {
            Date(timeIntervalSince1970: ($0.timeIntervalSince1970 / 300).rounded(.down) * 300)
        }
    }

    /// True while the only thing holding this alive is demo seeding. Kept in
    /// MEMORY and never written to disk, which is the whole trick: a demo
    /// beat that survived a relaunch could not be revoked (the flag that
    /// knows to revoke it wouldn't survive), so a stage prop would go on
    /// claiming a verified path over a real fleet — masking the very
    /// dead-man's-switch it was standing in for.
    @Published private(set) var isDemoFed = false

    /// The demo's stage prop: alive-looking, never persisted, and dropped by
    /// the first real signal or the first real device.
    func recordDemoBeat(now: Date = Date()) {
        lastBeat = now
        lastBeatSource = .pathVerified
        lastVerified = now
        // The prop stands in for the fleet too, so the demo's card doesn't
        // accuse a fleet that isn't there of going dark.
        lastFleetCheckIn = now
        isDemoFed = true
        clearTestVerdict()
        tick(now: now)
    }

    /// A signal arrived. `.pathVerified` is the strong claim (a delivery iOS
    /// accepted); `.fleetCheckIn` is the weak one (a Canary answered).
    func recordBeat(source: WristBeatSource = .pathVerified, now: Date = Date()) {
        // Coalesce the fleet's chatter; never a real verification, which is
        // rare, deliberate, and always worth stamping.
        if source == .fleetCheckIn, lastBeatSource == .fleetCheckIn, let last = lastBeat,
           now.timeIntervalSince(last) < Self.beatCoalescing {
            tick(now: now)
            return
        }
        lastBeat = now
        lastBeatSource = source
        if source == .pathVerified { lastVerified = now }
        // Only the fleet answering feeds the dead-man's-switch.
        if source == .fleetCheckIn { lastFleetCheckIn = now }
        // A real signal outranks the prop, and only real signals reach disk.
        isDemoFed = false
        persist()
        // A verified delivery is the ANSWER to a test — in flight or already
        // failed — so it clears that verdict before we re-evaluate. `tick`
        // deliberately leaves both alone, and without this the successful
        // Test Alert (the hero moment of this whole card) would sit on
        // "Testing the whole path…" forever.
        //
        // A fleet check-in clears NEITHER: it is not the test's answer, and
        // it does not fix whatever made the test fail.
        if source == .pathVerified { clearTestVerdict() }
        tick(now: now)
    }

    private func clearTestVerdict() {
        switch state {
        case .testing, .failed: state = .alive(secondsAgo: 0)
        default: break
        }
    }

    /// Forget everything (e.g. leaving demo mode) — back to "Not yet
    /// verified", never a green check that wasn't earned end-to-end.
    func reset() {
        lastVerified = nil
        lastBeat = nil
        lastBeatSource = nil
        lastFleetCheckIn = nil
        isDemoFed = false
        state = .unknown
        persist()
    }

    /// The app can hear again (foreground, radios started). Restarts the
    /// window the dark verdict is measured over — see guard 1 above.
    func noteListening(now: Date = Date()) {
        listeningSince = now
        tick(now: now)
    }

    /// Re-evaluate liveness; call on a timer and on foreground.
    func tick(now: Date = Date()) {
        // A test in flight owns the card until it answers.
        if case .testing = state { return }
        // So does a FAILED one, until the path actually works again. This
        // matters more now that verifications persist: without it, "Test
        // failed: notifications are off for SecuraCV" would be overwritten by
        // the next 20-second refresh with "Delivery verified 3 days ago" —
        // a stale success papering over a live failure the user just saw.
        // Only a fresh `.pathVerified` beat clears it (see `recordBeat`); a
        // Canary checking in does not, because it doesn't fix what broke.
        if case .failed = state { return }
        guard let last = lastBeat else { state = .unknown; return }
        let ago = Int(now.timeIntervalSince(last))
        // The dark verdict is about the FLEET's silence, so it is measured
        // from the fleet's last answer — never from a notification we
        // delivered — and only from the point we could hear at all.
        let listening = listeningSince ?? last
        let heardFrom = max(lastFleetCheckIn ?? listening, listening)
        let window = beatInterval * Double(missTolerance)
        if expectsBeats, now.timeIntervalSince(heardFrom) > window {
            // Report the silence in the fleet's own terms.
            let silentFor = lastFleetCheckIn.map { Int(now.timeIntervalSince($0)) } ?? ago
            state = .dark(sinceSeconds: silentFor)
        } else {
            state = .alive(secondsAgo: ago)
        }
    }

    /// One-tap self-test. `roundTrip` performs the actual device→relay→APNs→
    /// phone loop (injected so this stays testable and transport-agnostic).
    func runTestAlert(_ roundTrip: @escaping () async throws -> Void) async {
        state = .testing
        do {
            try await roundTrip()
            recordBeat(source: .pathVerified)
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    /// Human summary for the "provably alive" card. The wording lives in
    /// Shared/WristSnapshot.swift (HeartbeatCopy) because the watch renders
    /// the same sentence from the same facts — one copy of the truth.
    var summary: String {
        HeartbeatCopy.summary(state: wristState,
                              secondsSinceVerified: wristSecondsSinceVerified,
                              failureReason: wristFailureReason,
                              source: lastBeatSource)
    }

    /// The wire-flattened view of `state` for the wrist snapshot.
    var wristState: WristHeartbeatState {
        switch state {
        case .unknown: return .unknown
        case .alive: return .alive
        case .testing: return .testing
        case .dark: return .dark
        case .failed: return .failed
        }
    }

    private var wristSecondsSinceVerified: Int? {
        switch state {
        case .alive(let s): return s
        case .dark(let s): return s
        case .unknown, .testing, .failed: return nil
        }
    }

    /// Only meaningful when `state` is `.failed`.
    var wristFailureReason: String? {
        if case .failed(let why) = state { return why }
        return nil
    }

    // MARK: - persistence

    private func load() {
        if let t = defaults.object(forKey: Self.verifiedKey) as? Double {
            lastVerified = Date(timeIntervalSince1970: t)
        }
        if let t = defaults.object(forKey: Self.beatKey) as? Double {
            lastBeat = Date(timeIntervalSince1970: t)
        }
        if let raw = defaults.object(forKey: Self.beatSourceKey) as? Int {
            lastBeatSource = WristBeatSource(tolerant: raw)
        }
        // Don't claim anything yet: the first tick (with a real listening
        // window and a real expectsBeats) decides what this state means.
        state = .unknown
    }

    private func persist() {
        set(lastVerified.map(\.timeIntervalSince1970), forKey: Self.verifiedKey)
        set(lastBeat.map(\.timeIntervalSince1970), forKey: Self.beatKey)
        if let raw = lastBeatSource?.rawValue {
            defaults.set(Int(raw), forKey: Self.beatSourceKey)
        } else {
            defaults.removeObject(forKey: Self.beatSourceKey)
        }
    }

    private func set(_ value: Double?, forKey key: String) {
        if let value {
            defaults.set(value, forKey: key)
        } else {
            defaults.removeObject(forKey: key)
        }
    }
}

/// Does the fleet's current state count as a check-in? Pure so the rule is
/// testable and stated once: a device we can actually hear right now — over
/// Wi-Fi or over the air — is the fleet reporting in. Demo rows never count;
/// sample data must never feed a liveness claim.
enum FleetBeat {
    static func heard(in witnesses: [Witness], demoPrefix: String) -> Bool {
        witnesses.contains { !$0.id.hasPrefix(demoPrefix) && $0.link == .online }
    }
}
