// SecuraCVApp.swift — entry point.
//
// One @main, one shared FleetStore, a four-tab shell. Everything reacts to the
// scene phase so the app quiets its radios in the background and re-checks the
// moment it returns (a security app must never be caught stale).

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif
#if canImport(CloudKit)
import CloudKit
#endif

@main
struct SecuraCVApp: App {
    @StateObject private var store = FleetStore()
    @Environment(\.scenePhase) private var scenePhase
    // SwiftUI has no hook for the remote-notification callbacks, and the away
    // path is not optional polish — without a delegate the wake arrives and
    // nothing in the app ever learns it did.
    #if canImport(UIKit)
    @UIApplicationDelegateAdaptor(PushDelegate.self) private var pushDelegate
    #endif

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .tint(Theme.color(.info))
                .task {
                    #if canImport(UIKit)
                    PushDelegate.store = store
                    #endif
                    // The intents' landing pad (Siri / Shortcuts / Action
                    // button) — same pattern, same timing as the push one.
                    AppIntentBridge.store = store
                    // Persisted Apple Home consent resumes here — never in
                    // the bridge's init, so tests and previews stay dark
                    // (the lazy-manager rule in HomeKitBridge).
                    HomeKitBridge.shared.resumeIfEnabled()
                    store.onAppear()
                }
        }
        .onChange(of: scenePhase) { _, phase in
            store.onScenePhase(active: phase == .active)
        }
    }
}

#if canImport(UIKit)
/// The landing pad for APNs. Deliberately thin: it knows how to hand a wake
/// to the store and nothing else, so the away path's real logic stays in
/// AwayPush and FleetStore where it can be read and tested.
final class PushDelegate: NSObject, UIApplicationDelegate {
    /// Set once the scene exists. Static because iOS builds the delegate
    /// before SwiftUI builds the store, and a wake that arrives in that gap
    /// must not crash — it simply refreshes on the next foreground.
    @MainActor static weak var store: FleetStore?

    func application(_ application: UIApplication,
                     didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        // Nothing to send anywhere: CloudKit owns the token, and we run no
        // server that could want it. Registration exists only so the
        // subscription's pushes can be delivered at all.
    }

    func application(_ application: UIApplication,
                     didFailToRegisterForRemoteNotificationsWithError error: Error) {
        Task { @MainActor in
            await AwayPush.shared.noteRegistrationFailure()
        }
    }

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    /// Somebody tapped an invitation to help watch a fleet. iOS hands us the
    /// share metadata here and nowhere else — there is no URL to parse and no
    /// link of ours to handle, because the whole invitation flow belongs to
    /// Apple. Accepting is what turns this device into a participant.
    func application(_ application: UIApplication,
                     userDidAcceptCloudKitShareWith metadata: CKShare.Metadata) {
        Task { @MainActor in
            await HouseholdShare.shared.acceptInvitation(metadata)
        }
    }
    #endif

    /// A wake landed. The NSE already composed what the user sees; our job is
    /// to make the app's own state true — pull the fleet so that opening the
    /// notification shows the real thing, not the state from before the wake.
    func application(_ application: UIApplication,
                     didReceiveRemoteNotification userInfo: [AnyHashable: Any]) async
        -> UIBackgroundFetchResult {
        let wake = WakePayload.wakeClass(from: userInfo)
        return await MainActor.run {
            guard let store = Self.store else { return UIBackgroundFetchResult.noData }
            store.noteAwayWake(wake)
            return .newData
        }
    }
}
#endif

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
        Group {
            if horizontalSizeClass == .regular {
                SidebarRootView(section: $section)
            } else {
                TabRootView(section: $section)
            }
        }
        // The bird helper's bubble, docked over every section in both
        // idioms — one channel the whole app can rely on, always in the
        // same corner (the website Herald's promise, kept here). Mounted
        // at the root so a message can never be delivered off-screen. It
        // rides over the navigation-bar band for its dwell, the same
        // top-of-page overlay the site's docked bar is — acceptable because
        // messages are rare and paced, and the one sticky tone (warn)
        // carries its own dismiss.
        .overlay(alignment: .top) {
            CanaryVoiceBubble(voice: store.voice) { section = $0 }
        }
        .onAppear { store.voice.arrive(at: section) }
        // The deep-link doors, both landing on the same route value: a
        // widget or link speaks the securacv:// dialect; a notification tap
        // arrives already parsed (AlertCenter → pendingRoute). The shell's
        // only job is the tab switch — the destination view consumes the
        // route (and any anchor in it) itself, so routing logic never
        // leaks into the shell.
        .onOpenURL { url in
            if let route = AppRoute(url: url) { store.pendingRoute = route }
        }
        // Arriving on a section is the moment its one orientation line can
        // help — once per section, ever, and only where a newcomer might
        // stall (the tip table in CanaryVoiceStage).
        .onChange(of: section) { _, now in
            store.voice.orient(now)
        }
        .onChange(of: store.pendingRoute) { _, route in
            guard let route else { return }
            switch route {
            case .today:
                section = .today
                store.pendingRoute = nil   // no anchor to consume — done here
            case .alerts:
                section = .alerts          // AlertsView consumes and clears
            case .find:
                section = .fleet           // FleetView consumes and clears
            }
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
