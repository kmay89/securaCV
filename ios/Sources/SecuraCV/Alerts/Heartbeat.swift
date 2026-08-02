// Heartbeat.swift
//
// "Provably alive" — the smoke-alarm chirp, but for the whole delivery path.
// The fleet emits a signed heartbeat along the SAME route a real alarm takes;
// this tracks when we last saw one end-to-end and lets the user fire a one-tap
// Test Alert that round-trips device → relay → APNs → phone and lights green.
//
// It is ALSO the dead-man's-switch: if the heartbeat goes quiet past the "dark"
// window, the SILENCE is itself the alarm (docs §5b). Who watches the silence
// while the phone sleeps is a hub/peer/relay concern; this object is the phone's
// own view of liveness and the on-demand self-test.

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
    @Published private(set) var lastVerified: Date?

    /// Beats are expected on this cadence; miss `missTolerance` in a row → dark.
    var beatInterval: TimeInterval = 300      // 5 min
    var missTolerance: Int = 3

    /// Called whenever a signed heartbeat is confirmed (LAN, relay, or peer).
    func recordBeat() {
        lastVerified = Date()
        state = .alive(secondsAgo: 0)
    }

    /// Forget everything (e.g. leaving demo mode) — back to "Not yet
    /// verified", never a green check that wasn't earned end-to-end.
    func reset() {
        lastVerified = nil
        state = .unknown
    }

    /// Re-evaluate liveness; call on a timer and on foreground.
    func tick(now: Date = Date()) {
        guard let last = lastVerified else { state = .unknown; return }
        let ago = Int(now.timeIntervalSince(last))
        if Double(ago) > beatInterval * Double(missTolerance) {
            state = .dark(sinceSeconds: ago)
        } else if case .testing = state {
            // leave a test in flight alone
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
            recordBeat()
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
                              failureReason: wristFailureReason)
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
}
