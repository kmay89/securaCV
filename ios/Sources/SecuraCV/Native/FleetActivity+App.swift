// FleetActivity+App.swift
//
// App-target builder for the shared FleetActivityAttributes.State. The
// glance math itself lives in Model/FleetRollup.swift so the island and the
// wrist snapshot are computed from the same rules; this file only shapes the
// result into ActivityKit's payload. (The `severity` accessor lives with the
// shared attributes now that Severity itself is shared.)

import Foundation

extension FleetActivityAttributes.State {
    /// Build the live state from the current fleet snapshot.
    init(fleet: [Witness], lastVerifiedAgo: Int?) {
        self.init(severityRaw: FleetRollup.worst(fleet).rawValue,
                  headline: FleetRollup.headline(fleet),
                  healthy: FleetRollup.healthyCount(fleet),
                  total: fleet.count,
                  lastVerifiedAgo: lastVerifiedAgo)
    }
}
