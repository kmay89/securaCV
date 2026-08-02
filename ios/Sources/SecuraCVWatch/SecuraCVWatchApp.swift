// SecuraCVWatchApp.swift — SecuraCV on your wrist (entry point).
//
// The 3-screen scope from the RFC (§3.3), no more: fleet glance, heartbeat,
// about. The phone's FleetStore is the source of truth; this app renders the
// WristSnapshot it sends and asks for freshness at the moments that matter
// (launch, foreground, reachability). No pairing, no key custody, no video —
// phone territory and invariants, respectively.

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
            HeartbeatView()
            AboutView()
        }
        .tabViewStyle(.verticalPage)
    }
}
