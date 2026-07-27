// SecuraCVApp.swift — entry point.
//
// One @main, one shared FleetStore, a four-tab shell. Everything reacts to the
// scene phase so the app quiets its radios in the background and re-checks the
// moment it returns (a security app must never be caught stale).

import SwiftUI

@main
struct SecuraCVApp: App {
    @StateObject private var store = FleetStore()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .tint(Theme.color(.info))
                .task { store.onAppear() }
        }
        .onChange(of: scenePhase) { _, phase in
            store.onScenePhase(active: phase == .active)
        }
    }
}

struct RootView: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        TabView {
            TodayView()
                .tabItem { Label("Today", systemImage: "sparkles") }
            FleetView()
                .tabItem { Label("Fleet", systemImage: "bird") }
            AlertsView()
                .tabItem { Label("Alerts", systemImage: "bell.badge") }
            KeysView()
                .tabItem { Label("Keys", systemImage: "key.horizontal") }
        }
    }
}
