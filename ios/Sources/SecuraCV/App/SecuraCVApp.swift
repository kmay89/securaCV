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
    // Owned HERE, above the idiom switch, so a window crossing the
    // regular/compact boundary (a Split View drag, a Stage Manager resize,
    // rotating a Max phone) stays on the section in use — swapping the
    // container must never navigate the user away.
    @State private var section: AppSection = .today

    var body: some View {
        if horizontalSizeClass == .regular {
            SidebarRootView(section: $section)
        } else {
            TabRootView(section: $section)
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
    @Binding var section: AppSection

    var body: some View {
        TabView(selection: $section) {
            ForEach(AppSection.allCases) { s in
                AppSectionView(section: s)
                    .tabItem { Label(s.title, systemImage: s.systemImage) }
                    .tag(s)
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
    @Binding var section: AppSection

    /// The sidebar list wants an optional selection; the app never has "no
    /// section", so a nil set (a transient deselection) keeps the last one.
    private var selection: Binding<AppSection?> {
        Binding(get: { section }, set: { if let s = $0 { section = s } })
    }

    var body: some View {
        NavigationSplitView {
            List(AppSection.allCases, selection: selection) { s in
                HStack {
                    Label(s.title, systemImage: s.systemImage)
                    Spacer()
                    if s == .fleet && !store.allQuiet {
                        SeverityPip(severity: store.worstSeverity)
                    }
                }
                .tag(s)
            }
            .navigationTitle("SecuraCV")
        } detail: {
            AppSectionView(section: section)
        }
    }
}

#Preview("Sidebar (iPad) — demo fleet") {
    struct Host: View {
        @State private var section: AppSection = .today
        var body: some View {
            SidebarRootView(section: $section)
                .environmentObject(DemoFleet.previewStore())
        }
    }
    return Host()
}
