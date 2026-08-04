// FleetIntents.swift
//
// "Ask, don't open." The four verbs the app exports to the rest of the
// ecosystem as App Intents — Siri, the Shortcuts app, Spotlight, the Action
// button, and Focus/location automations all speak them with zero setup:
//
//   * Check the Fleet   — the one honest answer, spoken from the glance
//                         cache; no app launch, no radio, instant.
//   * Test Alert Path   — the alert self-test (docs §5b): posts a real
//                         notification and confirms iOS accepted it. Opens
//                         the app: the proof should be seen and heard, not
//                         narrated. The verdict claims only what the test
//                         proved (rule 4: don't overclaim).
//   * Quiet Hour        — every paired Canary muted for an hour, in one
//                         verb. Tamper and failed signatures still punch
//                         through (Witness.effectiveSeverity owns that; no
//                         intent path can weaken it).
//   * Resume Alerts     — the symmetric verb, so quiet is never a trap.
//
// This is how the app stays a daily touchpoint without becoming a daily
// chore: the platform asks on the user's behalf — from the wrist, the
// Action button, an automation — and the app answers in one sentence. All
// wording comes from Shared/GlanceAnswer.swift (host-tested); intents never
// invent copy.
//
// Liveness rule: intents that only READ answer from PhoneGlanceCache and
// never launch anything. Intents that ACT prefer the live store (instant
// UI, wrist, and island updates) and fall back to the same durable ledgers
// the store folds on next launch — so a verb spoken to a cold phone still
// lands, exactly once, with no duplicate path to drift.

import AppIntents
import SwiftUI

/// The intents' bridge to the live store. Set beside PushDelegate.store the
/// moment the scene exists; nil when iOS runs an intent without building the
/// UI (background Shortcuts), in which case acting intents use the ledgers.
@MainActor
enum AppIntentBridge {
    static weak var store: FleetStore?

    /// Wait briefly for a cold-started scene to finish wiring — an
    /// openAppWhenRun intent races the root view's .task by design.
    static func waitForStore() async -> FleetStore? {
        for _ in 0..<20 where store == nil {
            try? await Task.sleep(for: .milliseconds(250))
        }
        return store
    }
}

struct CheckFleetIntent: AppIntent {
    static let title: LocalizedStringResource = "Check the Fleet"
    static let description = IntentDescription(
        "The fleet's one honest answer, without opening the app.")

    func perform() async throws -> some IntentResult & ProvidesDialog & ShowsSnippetView {
        let snapshot = PhoneGlanceCache.load()
        let sentence = GlanceAnswer.spoken(snapshot)
        return .result(dialog: IntentDialog(stringLiteral: sentence),
                       view: FleetAnswerSnippet(snapshot: snapshot))
    }
}

struct TestAlertPathIntent: AppIntent {
    static let title: LocalizedStringResource = "Test Alert Path"
    static let description = IntentDescription(
        "Run the alert self-test — post a real notification and confirm it can reach you.")
    /// The test posts a real notification and earns a real chirp — it should
    /// happen in front of the user, on the provably-alive card.
    static let openAppWhenRun = true

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        guard let store = await AppIntentBridge.waitForStore() else {
            return .result(dialog: "SecuraCV is still waking up — try again in a moment.")
        }
        await store.runTestAlert()
        let verdict = GlanceAnswer.pathTest(verified: store.heartbeat.state.isHealthy,
                                            summary: store.heartbeat.summary)
        return .result(dialog: IntentDialog(stringLiteral: verdict))
    }
}

struct QuietHourIntent: AppIntent {
    static let title: LocalizedStringResource = "Quiet Hour"
    static let description = IntentDescription(
        "Mute everyday alerts from every Canary for an hour. Tamper and signature failures still come through.")

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let count: Int
        if let store = AppIntentBridge.store {
            count = store.quietFleet()
        } else {
            // Cold phone: write the same durable ledger the store re-applies
            // at every fold. The durable paired list is the source of truth —
            // the glance cache caps at WristSync.maxWitnessRows and may omit
            // rows, and a Quiet Hour that silently skipped Canary #25 would
            // be the lie this app refuses to tell. Cached glance ids are
            // unioned in so provisional rows get their hour too; demo rows
            // excluded so sample data never inflates the honest count.
            let paired = DeviceStore().devices.map(\.id)
            let glanced = PhoneGlanceCache.load()?.witnesses.map(\.id) ?? []
            let ids = Set(paired + glanced).filter { !$0.hasPrefix(DemoFleet.idPrefix) }
            let ledger = MuteLedger()
            let until = Date().addingTimeInterval(3600)
            for id in ids { ledger.set(until: until, for: id) }
            count = ids.count
        }
        return .result(dialog: IntentDialog(stringLiteral: GlanceAnswer.quieted(count: count)))
    }
}

struct ResumeAlertsIntent: AppIntent {
    static let title: LocalizedStringResource = "Resume Alerts"
    static let description = IntentDescription(
        "End the quiet hour early — every mute cleared, alerts back to full volume.")

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let count: Int
        if let store = AppIntentBridge.store {
            count = store.resumeFleet()
        } else {
            let ledger = MuteLedger()
            count = ledger.activeMutes().count
            ledger.clearAll()
        }
        return .result(dialog: IntentDialog(stringLiteral: GlanceAnswer.resumed(count: count)))
    }
}

/// Zero-setup surfacing: these phrases make the verbs live in Siri, the
/// Shortcuts gallery, Spotlight, and the Action button picker the moment the
/// app is installed — no in-app switch, no configuration screen to rot.
struct SecuraCVShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: CheckFleetIntent(),
                    phrases: [
                        "Check \(.applicationName)",
                        "How is my fleet in \(.applicationName)",
                        "Is everything okay in \(.applicationName)",
                    ],
                    shortTitle: "Check the Fleet",
                    systemImageName: "bird")
        AppShortcut(intent: TestAlertPathIntent(),
                    phrases: [
                        "Test my alerts in \(.applicationName)",
                        "Prove \(.applicationName) can reach me",
                    ],
                    shortTitle: "Test Alert Path",
                    systemImageName: "checkmark.seal")
        AppShortcut(intent: QuietHourIntent(),
                    phrases: [
                        "Quiet \(.applicationName) for an hour",
                        "Start a quiet hour in \(.applicationName)",
                    ],
                    shortTitle: "Quiet Hour",
                    systemImageName: "moon.zzz")
        AppShortcut(intent: ResumeAlertsIntent(),
                    phrases: [
                        "Resume alerts in \(.applicationName)",
                    ],
                    shortTitle: "Resume Alerts",
                    systemImageName: "bell")
    }
}

/// The card under Siri's spoken answer — the same story the widgets tell,
/// compressed to one row. Deliberately tiny: the snippet supports the
/// sentence; it never becomes a second dashboard.
private struct FleetAnswerSnippet: View {
    let snapshot: WristSnapshot?

    var body: some View {
        HStack(spacing: Theme.s) {
            Image(systemName: snapshot?.severity.sfSymbol ?? "bird")
                .font(.title2)
                .foregroundStyle(Theme.color(snapshot.map { $0.severity.role } ?? .neutral))
            VStack(alignment: .leading, spacing: 1) {
                Text(snapshot?.headline ?? "Open SecuraCV once to link your fleet.")
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(2)
                if let snap = snapshot {
                    HStack(spacing: Theme.xs) {
                        Text("\(snap.healthy) of \(snap.total) healthy").monospacedDigit()
                        if snap.isDemoData { Text("· sample") }
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(Theme.m)
    }
}
