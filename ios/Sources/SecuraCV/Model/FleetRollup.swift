// FleetRollup.swift
//
// The one place that turns a fleet of Witnesses into the three glanceable
// numbers every ambient surface shows: worst severity, healthy count, and a
// headline. The Dynamic Island state and the wrist snapshot are both built
// from THIS, so the phone's island and the watch can never disagree about
// what "healthy" means. Pure Foundation — host-testable, no UI.

import Foundation

enum FleetRollup {
    /// A Canary counts as healthy when we can hear it, it isn't flagging
    /// tamper, and its chain signature didn't fail. Mute never changes
    /// health — mute caps *nagging*, not truth.
    static func isHealthy(_ w: Witness) -> Bool {
        !w.link.isDark && !w.tamper && w.badge != .failed
    }

    static func worst(_ fleet: [Witness]) -> Severity {
        fleet.map(\.effectiveSeverity).max() ?? .ok
    }

    static func healthyCount(_ fleet: [Witness]) -> Int {
        fleet.filter(isHealthy).count
    }

    /// "All quiet", or "<hottest witness> • <severity label>".
    static func headline(_ fleet: [Witness]) -> String {
        let sev = worst(fleet)
        if sev == .ok { return "All quiet" }
        if let hot = fleet.max(by: { $0.effectiveSeverity < $1.effectiveSeverity }) {
            return "\(hot.displayName) • \(sev.label)"
        }
        return sev.label
    }
}
