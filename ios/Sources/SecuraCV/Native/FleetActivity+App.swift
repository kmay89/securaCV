// FleetActivity+App.swift
//
// App-target accessor over the shared FleetActivityAttributes.State. The widget
// target defines its own equivalent; keeping them separate is what lets the
// shared attributes struct stay a pure, dependency-free data type.

import Foundation

extension FleetActivityAttributes.State {
    /// The app's Severity ladder view of the raw byte.
    var severity: Severity { Severity(tolerant: Int(severityRaw)) }

    /// Build the live state from the current fleet snapshot.
    init(fleet: [Witness], lastVerifiedAgo: Int?) {
        let worst = fleet.map(\.effectiveSeverity).max() ?? .ok
        let healthy = fleet.filter { !$0.link.isDark && !$0.tamper && $0.badge != .failed }.count
        let headline: String
        if worst == .ok {
            headline = "All quiet"
        } else if let hot = fleet.max(by: { $0.effectiveSeverity < $1.effectiveSeverity }) {
            headline = "\(hot.displayName) • \(worst.label)"
        } else {
            headline = worst.label
        }
        self.init(severityRaw: worst.rawValue,
                  headline: headline,
                  healthy: healthy,
                  total: fleet.count,
                  lastVerifiedAgo: lastVerifiedAgo)
    }
}
