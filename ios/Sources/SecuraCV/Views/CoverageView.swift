// CoverageView.swift — "would I be told?", answered on one screen.
//
// Renders `Coverage` and adds nothing to it. Every sentence here comes from
// the model, so what the user reads and what a test asserts are the same
// words. Reached from Alerts, because that is where someone is standing when
// the question occurs to them.

import SwiftUI

/// The one place that binds the live sources to the pure model.
///
/// Both the row in Alerts and the sheet it opens read the verdict from here,
/// so a summary line and the screen behind it can never disagree — a drift
/// that would undermine the exact promise this feature makes.
enum CoverageSource {
    @MainActor
    static func current(alerts: AlertCenter) -> Coverage {
        Coverage.evaluate(
            notificationsAuthorized: alerts.authorized,
            anyRuleArmed: alerts.rules.contains(where: \.enabled),
            awayReachReady: AwayPush.shared.reach.isReady,
            awayReachExplanation: AwayPush.shared.reach.explanation,
            homeKitEnabled: HomeKitBridge.shared.isEnabled,
            homeKitHubPresent: HomeKitBridge.shared.homeHubPresent,
            // Ours are anchored by UUID, so this is an observation. A
            // household's hand-written automation is not visible to us, and
            // the lane's copy says so rather than assuming either way.
            homeKitAutomationCount: HomeKitBridge.shared.authoredAutomations().count,
            // The phone cannot see another device's setting. Saying "we can't
            // check this from here" is the honest rung, and the model has one.
            residentKnown: false
        )
    }
}

/// The entry point, worn where the question occurs to someone: a row that
/// already answers it, and opens the detail if the answer needs explaining.
struct CoverageRow: View {
    @EnvironmentObject var store: FleetStore
    @ObservedObject private var away = AwayPush.shared
    @ObservedObject private var home = HomeKitBridge.shared
    @State private var showingCoverage = false

    var body: some View {
        let coverage = CoverageSource.current(alerts: store.alerts)
        Button { showingCoverage = true } label: {
            HStack(spacing: Theme.s) {
                Image(systemName: coverage.workingCount == 0
                      ? "exclamationmark.triangle" : "shield.lefthalf.filled")
                    .foregroundStyle(coverage.workingCount == 0
                                     ? Theme.color(.warn) : Theme.color(.calm))
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Would I be told?").font(.body)
                    Text(coverage.headline)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: Theme.s)
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.tertiary)
                    .accessibilityHidden(true)
            }
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityHint("Shows every path an alert could take to you.")
        .sheet(isPresented: $showingCoverage) {
            CoverageView().environmentObject(store)
        }
    }
}

struct CoverageView: View {
    @EnvironmentObject var store: FleetStore
    @ObservedObject private var away = AwayPush.shared
    @ObservedObject private var home = HomeKitBridge.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section {
                    VStack(alignment: .leading, spacing: Theme.xs) {
                        Text(coverage.headline)
                            .font(.headline)
                        Text(coverage.summary)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, Theme.xs)
                    .accessibilityElement(children: .combine)
                }

                Section {
                    ForEach(coverage.lanes) { lane in
                        laneRow(lane)
                    }
                } header: {
                    Text("The paths")
                } footer: {
                    Text("More than one on purpose: these fail independently, so a broken iCloud, a missing home hub, or an Apple outage each take out one path and not the rest.")
                }
            }
            .navigationTitle("Would I be told?")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private var coverage: Coverage {
        // `away` and `home` are observed above so this recomputes when they
        // change; the binding itself lives once, in CoverageSource.
        CoverageSource.current(alerts: store.alerts)
    }

    @ViewBuilder
    private func laneRow(_ lane: CoverageLane) -> some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            Label(lane.name, systemImage: symbol(for: lane.standing))
                .foregroundStyle(tint(for: lane.standing))
            Text(lane.carries)
                .font(.caption)
                .foregroundStyle(.secondary)
            if let note = note(for: lane.standing) {
                Text(note)
                    .font(.caption)
                    .foregroundStyle(tint(for: lane.standing))
            }
        }
        .padding(.vertical, 2)
        .accessibilityElement(children: .combine)
    }

    private func symbol(for standing: CoverageLane.Standing) -> String {
        switch standing {
        case .covered: return "checkmark.circle"
        case .broken: return "exclamationmark.triangle"
        case .off: return "circle"
        case .unobservable: return "questionmark.circle"
        }
    }

    private func tint(for standing: CoverageLane.Standing) -> Color {
        switch standing {
        case .covered: return Theme.color(.calm)
        case .broken: return Theme.color(.warn)
        case .off, .unobservable: return .secondary
        }
    }

    private func note(for standing: CoverageLane.Standing) -> String? {
        switch standing {
        case .covered: return nil
        case .broken(let why), .off(let why), .unobservable(let why): return why
        }
    }
}

#Preview("Coverage — demo fleet") {
    CoverageView().environmentObject(DemoFleet.previewStore())
}

#Preview("Coverage row") {
    List { CoverageRow().environmentObject(DemoFleet.previewStore()) }
}
