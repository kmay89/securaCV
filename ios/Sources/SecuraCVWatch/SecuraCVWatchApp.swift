// SecuraCVWatchApp.swift — SecuraCV on your wrist (entry point).
//
// The RFC's 3-screen scope (§3.3) plus exactly one: fleet glance, ALERTS,
// heartbeat, about. The fourth earns its place because the wrist was missing
// an answer to "what needed me while I wasn't looking?" — the question a
// watch is best at and the glance deliberately doesn't answer (it shows now,
// not history). Still no pairing, no key custody, no video — phone territory
// and invariants, respectively.
//
// The phone's FleetStore stays the source of truth; this app renders the
// WristSnapshot it sends and asks for freshness at the moments that matter
// (launch, foreground, reachability).

import SwiftUI

@main
struct SecuraCVWatchApp: App {
    @StateObject private var store = WristStore()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            WristRootView()
                .environmentObject(store)
                .tint(Theme.color(.info))
                .task { store.activate() }
        }
        .onChange(of: scenePhase) { _, phase in
            // A security surface must never be caught stale: every return to
            // the foreground asks the phone for current truth.
            if phase == .active { store.requestRefresh() }
        }
    }
}

struct WristRootView: View {
    @EnvironmentObject var store: WristStore

    var body: some View {
        TabView {
            FleetGlanceView()
            // Second, right after the glance: "how is the fleet now?" and
            // "what needed me?" are the two questions a wrist gets asked, and
            // the second one used to have no answer anywhere on the watch.
            AlertsListView()
            HeartbeatView()
            AboutView()
        }
        .tabViewStyle(.verticalPage)
    }
}
