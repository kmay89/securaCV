// WristSnapshot+App.swift
//
// App-target builder for the shared WristSnapshot: folds the FleetStore's
// current truth into the wire shape. The roll-up trio comes from
// Model/FleetRollup.swift — the same math as the Dynamic Island — and every
// timestamp is coarsened with the same bucket the timeline uses
// (Invariant III applies on the wire too). `revision`/`sentAt` are stamped by
// WatchLink at send time, not here, so building a snapshot for comparison is
// free of side effects.

import Foundation

extension WristSnapshot {
    @MainActor
    init(store: FleetStore) {
        let fleet = store.witnesses   // already sorted worst-first
        let rows = fleet.prefix(WristSync.maxWitnessRows).map { w in
            WristWitness(id: w.id,
                         name: w.displayName,
                         severityRaw: w.effectiveSeverity.rawValue,
                         linkRaw: w.link.rawValue,
                         badgeRaw: w.badge.rawValue,
                         tamper: w.tamper,
                         lastEventHeadline: w.lastEvent.isEmpty ? nil : w.lastEvent,
                         lastEventBucket: w.lastEventAt.map(FleetStore.bucket),
                         batteryPct: w.batteryPct,
                         isMuted: w.isMuted)
        }
        self.init(revision: 0,
                  sentAt: Date(timeIntervalSince1970: 0),
                  fleetName: store.fleetName,
                  severityRaw: FleetRollup.worst(fleet).rawValue,
                  headline: FleetRollup.headline(fleet),
                  healthy: FleetRollup.healthyCount(fleet),
                  total: fleet.count,
                  lastVerifiedAt: store.heartbeat.lastVerified,
                  heartbeatRaw: store.heartbeat.wristState.rawValue,
                  heartbeatFailureReason: store.heartbeat.wristFailureReason,
                  isDemoData: store.demoMode,
                  witnesses: Array(rows),
                  omittedWitnesses: max(0, fleet.count - rows.count))
    }
}
