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
