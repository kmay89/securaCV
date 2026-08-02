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
    // The one iPhone-vs-iPad decision, made at the root: compact width gets
    // the tab bar, regular width (iPad, and the big iPhones in landscape)
    // gets the platform's sidebar idiom. Every section is the SAME view
    // either way — designed FOR iPad never means a second implementation
    // (the two-flashers lesson, kept on purpose).
    @Environment(\.horizontalSizeClass) private var horizontalSizeClass

    var body: some View {
        if horizontalSizeClass == .regular {
            SidebarRootView()
        } else {
            TabRootView()
        }
    }
}

/// The four surfaces, named once — the tab bar and the sidebar both render
/// this list, so the two idioms can never diverge on what the app contains.
enum AppSection: String, CaseIterable, Identifiable {
    case today, fleet, alerts, keys

    var id: String { rawValue }

    var title: String {
        switch self {
        case .today: return "Today"
        case .fleet: return "Fleet"
        case .alerts: return "Alerts"
        case .keys: return "Keys"
        }
    }

    var systemImage: String {
        switch self {
        case .today: return "sparkles"
        case .fleet: return "bird"
        case .alerts: return "bell.badge"
        case .keys: return "key.horizontal"
        }
    }
}

/// A section's content — shared by both root idioms.
struct AppSectionView: View {
    let section: AppSection
    var body: some View {
        switch section {
        case .today: TodayView()
        case .fleet: FleetView()
        case .alerts: AlertsView()
        case .keys: KeysView()
        }
    }
}

struct TabRootView: View {
    var body: some View {
        TabView {
            ForEach(AppSection.allCases) { section in
                AppSectionView(section: section)
                    .tabItem { Label(section.title, systemImage: section.systemImage) }
            }
        }
    }
}

/// The iPad shape: a persistent sidebar with the worst-severity pip beside
/// Fleet when something needs a look — the big canvas earns a glanceable
/// answer before a single tap. Each section keeps its own NavigationStack in
/// the detail column, so deep links and back-stacks behave exactly as they
/// do inside the tabs.
struct SidebarRootView: View {
    @EnvironmentObject var store: FleetStore
    @State private var selection: AppSection? = .today

    var body: some View {
        NavigationSplitView {
            List(AppSection.allCases, selection: $selection) { section in
                HStack {
                    Label(section.title, systemImage: section.systemImage)
                    Spacer()
                    if section == .fleet && !store.allQuiet {
                        SeverityPip(severity: store.worstSeverity)
                    }
                }
                .tag(section)
            }
            .navigationTitle("SecuraCV")
        } detail: {
            AppSectionView(section: selection ?? .today)
        }
    }
}

#Preview("Sidebar (iPad) — demo fleet") {
    SidebarRootView().environmentObject(DemoFleet.previewStore())
}
