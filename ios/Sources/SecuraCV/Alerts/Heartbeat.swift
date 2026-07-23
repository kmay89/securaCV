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

    /// Human summary for the "provably alive" card.
    var summary: String {
        switch state {
        case .unknown: return "Not yet verified"
        case .alive(let s):
            if s < 90 { return "Delivery verified just now" }
            return "Delivery verified \(s / 60) min ago"
        case .testing: return "Testing the whole path…"
        case .dark(let s): return "No heartbeat for \(s / 60) min — check your fleet"
        case .failed(let why): return "Test failed: \(why)"
        }
    }
}
