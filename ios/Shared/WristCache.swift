// WristCache.swift  (SHARED — written by the watch app, read by its widgets)
//
// The watch-LOCAL app-group cache behind the complications. App groups do NOT
// sync containers between iPhone and Watch (RFC §3.2 — the hard constraint
// this whole pipeline exists to route around), so this group is a second,
// wrist-side one: the watch app receives a WristSnapshot over WCSession and
// parks it here; the widget timeline renders from here even when the app
// isn't running. The iPhone app compiles this file but never uses it.
//
// Storage is UserDefaults-in-a-suite rather than a file: the payload is one
// small JSON blob, and defaults give us atomicity for free.

import Foundation

enum WristCache {
    /// Watch-side app group — declared in Support/Watch.entitlements and
    /// Support/WatchWidgets.entitlements (and ONLY those two).
    static let appGroupID = "group.com.securacv.witness.watch"
    static let snapshotKey = "wrist_snapshot_v1"

    static func save(_ snapshot: WristSnapshot, to defaults: UserDefaults? = nil) {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = try? WristSync.makeEncoder().encode(snapshot) else { return }
        store.set(data, forKey: snapshotKey)
    }

    static func load(from defaults: UserDefaults? = nil) -> WristSnapshot? {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = store.data(forKey: snapshotKey) else { return nil }
        return try? WristSync.makeDecoder().decode(WristSnapshot.self, from: data)
    }
}

/// The last Canary the user went FINDING for, remembered watch-side so the
/// Find complication has a one-tap target. Written by the wrist's Find
/// screen when a search actually starts, read by the widget provider — the
/// same app-group discipline as the snapshot cache above.
enum WristLastFind {
    static let key = "wrist_last_find_id_v1"
    /// The Find complication's kind — the app reloads exactly this timeline
    /// when a new search starts, so the face shows the new target now.
    static let widgetKind = "SecuraCVFindCanary"

    static func remember(_ witnessID: String, in defaults: UserDefaults? = nil) {
        (defaults ?? UserDefaults(suiteName: WristCache.appGroupID))?
            .set(witnessID, forKey: key)
    }

    static func load(from defaults: UserDefaults? = nil) -> String? {
        (defaults ?? UserDefaults(suiteName: WristCache.appGroupID))?
            .string(forKey: key)
    }

    /// The row the complication may honestly deep-link to — the remembered
    /// id, IF every gate the Find screen itself enforces would let a search
    /// actually start: discovery consent granted (the phone's choice, in
    /// the snapshot), the row still present, its beacon recognizable
    /// (fingerprint known), and no suffix twin (the screen refuses the
    /// signal for twins). Anything less and the complication carries no
    /// link and opens the app plainly, instead of a "Find <name>" tap that
    /// lands on a refusal message. Pure, host-tested.
    static func findableTarget(id: String?, in snapshot: WristSnapshot?) -> WristWitness? {
        guard let id, let snapshot, snapshot.discoveryConsented == true else { return nil }
        return snapshot.witnesses.first {
            $0.id == id && $0.fingerprint != nil && $0.suffixAmbiguous != true
        }
    }
}

/// The PHONE-side twin: written by FleetStore into the iPhone app group
/// (`group.com.securacv.witness` — the one the app and widget entitlements
/// have carried since day one, now actually earning its keep), read by the
/// iPhone Lock Screen / Home Screen widgets, and — next — by the NSE when it
/// hydrates a content-free push. Same WristSnapshot shape everywhere: one
/// glance contract for every ambient surface.
enum PhoneGlanceCache {
    static let appGroupID = "group.com.securacv.witness"
    static let snapshotKey = "fleet_glance_v1"
    /// The iPhone glance widget's kind string — FleetStore reloads exactly
    /// this timeline when the cached truth changes.
    static let widgetKind = "SecuraCVFleetGlancePhone"

    static func save(_ snapshot: WristSnapshot, to defaults: UserDefaults? = nil) {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = try? WristSync.makeEncoder().encode(snapshot) else { return }
        store.set(data, forKey: snapshotKey)
    }

    static func load(from defaults: UserDefaults? = nil) -> WristSnapshot? {
        guard let store = defaults ?? UserDefaults(suiteName: appGroupID),
              let data = store.data(forKey: snapshotKey) else { return nil }
        return try? WristSync.makeDecoder().decode(WristSnapshot.self, from: data)
    }
}
