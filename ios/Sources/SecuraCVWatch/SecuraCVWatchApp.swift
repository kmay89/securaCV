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

/// The four vertical pages, named so a deep link can land on one.
enum WristTab: Hashable {
    case glance, alerts, heartbeat, about
}

struct WristRootView: View {
    @EnvironmentObject var store: WristStore
    @State private var tab: WristTab = .glance

    var body: some View {
        TabView(selection: $tab) {
            FleetGlanceView().tag(WristTab.glance)
            // Second, right after the glance: "how is the fleet now?" and
            // "what needed me?" are the two questions a wrist gets asked, and
            // the second one used to have no answer anywhere on the watch.
            AlertsListView().tag(WristTab.alerts)
            HeartbeatView().tag(WristTab.heartbeat)
            AboutView().tag(WristTab.about)
        }
        .tabViewStyle(.verticalPage)
        // The complication's door. Same securacv:// dialect as the phone
        // (Shared/AppRoute — one vocabulary, host-tested), same division of
        // labor as the phone's shell: this switches the page, the glance
        // view consumes the find anchor and pushes the search itself.
        .onOpenURL { url in
            guard let route = AppRoute(url: url) else { return }
            switch route {
            case .today:
                tab = .glance
            case .alerts:
                tab = .alerts
            case .find(let witnessID):
                store.pendingFindID = witnessID
                tab = .glance
            }
        }
    }
}
