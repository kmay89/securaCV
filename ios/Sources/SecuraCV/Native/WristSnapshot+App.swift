// WristSnapshot+App.swift
//
// App-target builder for the shared WristSnapshot: folds the FleetStore's
// current truth into the wire shape. The roll-up trio comes from
// Model/FleetRollup.swift — the same math as the Dynamic Island. Witness
// EVENT times are coarsened with the same bucket the timeline uses
// (Invariant III applies on the wire too); link-health times keep their
// operational precision — see the two-kinds-of-time rule in
// Shared/WristSnapshot.swift. `revision`/`sentAt` are stamped by WatchLink
// at send time, not here, so building a snapshot for comparison is free of
// side effects.

import Foundation

extension WristSnapshot {
    @MainActor
    init(store: FleetStore) {
        let fleet = store.witnesses   // already sorted worst-first
        // The twin verdict is computed HERE, against the FULL fleet: the
        // rows below are capped, and a wrist-side check over the capped list
        // would miss a twin the cap dropped — then range toward the wrong
        // device while saying "Right here".
        let allFingerprints = fleet.map(\.fingerprint)
        let rows = fleet.prefix(WristSync.maxWitnessRows).map { w in
            WristWitness(id: w.id,
                         name: w.displayName,
                         severityRaw: w.effectiveSeverity.rawValue,
                         linkRaw: w.link.rawValue,
                         badgeRaw: w.badge.rawValue,
                         tamper: w.tamper,
                         tamperNarration: w.tamperKind.isEmpty ? nil : w.tamperKind,
                         lastEventHeadline: w.lastEvent.isEmpty ? nil : w.lastEvent,
                         lastEventBucket: w.lastEventAt.map(FleetStore.bucket),
                         batteryPct: w.batteryPct,
                         isMuted: w.isMuted,
                         // What lets the WRIST tie a heard beacon to this
                         // row (the beacon carries the fingerprint's last
                         // two bytes) — the key to finding on the watch's
                         // own radio.
                         fingerprint: w.fingerprint.isEmpty ? nil : w.fingerprint,
                         suffixAmbiguous: w.fingerprint.isEmpty ? nil :
                            ProximityRanger.isSuffixAmbiguous(fingerprint: w.fingerprint,
                                                              among: allFingerprints),
                         // The phone's own answer — the wrist can't see which
                         // devices this iPhone can reach over Wi-Fi.
                         canIdentify: store.canIdentify(w),
                         // What it IS: the wrist resolves the figure and the
                         // product name from these, through the same shared
                         // resolvers the phone and the Wall use. A row with
                         // no published type sends the coarse family's wire
                         // name, so the wrist still draws what the phone does.
                         publishedType: w.publishedType
                            ?? (w.deviceType == .unknown ? nil : w.deviceType.rawValue),
                         hardware: w.hardware,
                         hubRaw: w.hub == .unknown ? nil : w.hub.rawValue,
                         radarPresent: w.radarPresent,
                         radarOccupants: w.radarOccupants,
                         breathingLock: w.breathingLock)
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
                  omittedWitnesses: max(0, fleet.count - rows.count),
                  faceRaw: store.canaryFace.rawValue,
                  postureRaw: store.canaryPosture.rawValue,
                  trustDays: store.canaryTrustDays,
                  moodLine: store.moodLine,
                  // Cap-aware order: rows that still need a human always
                  // make the cut; settled history fills what's left
                  // (AlertHistory.wristRows — a live alarm must never fall
                  // off the end behind twelve settled rows).
                  alerts: AlertHistory.wristRows(store.alertLog.records,
                                                 cap: WristSync.maxAlertRows)
                      .map { r in
                          WristAlert(id: r.id,
                                     witnessID: r.witnessID,
                                     name: r.name,
                                     headline: r.headline,
                                     severityRaw: r.severityRaw,
                                     // Already a 10-minute bucket on the
                                     // phone; it crosses the wire unchanged.
                                     bucket: r.lastBucket,
                                     count: r.count,
                                     deliveryRaw: r.deliveryRaw,
                                     handlingRaw: r.handlingRaw,
                                     resolved: !r.isOpen)
                      },
                  // What the last beat actually proved, so the wrist's
                  // heartbeat screen says "your fleet checked in" where the
                  // phone would — the honesty split travels with the data.
                  lastBeatAt: store.heartbeat.wireLastBeat,
                  beatSourceRaw: store.heartbeat.lastBeatSource?.rawValue,
                  // The consent-first discovery choice travels with the
                  // data: the wrist's Find runs its own radio only behind
                  // the same gate the phone's radios honor.
                  discoveryConsented: store.discoveryConsent)
    }
}
