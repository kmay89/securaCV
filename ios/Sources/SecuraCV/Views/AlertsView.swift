// AlertsView.swift
//
// The tab answers the question people actually open it with: **did something
// need me, and did it reach me?** It used to answer a different one — it was a
// rules editor wearing the word "Alerts", so the app's own history of alarms
// existed nowhere, and the reach picker promised an away path the app had not
// built. Both are fixed here: history is the tab, rules moved behind one
// plainly-worded button, and every claim about reach comes from AwayPush's
// real capability instead of a segmented control's optimism.
//
// The shape, top to bottom:
//   * The heartbeat card stays the hero — proof the path works BEFORE the
//     emergency is the most valuable thing this screen can show.
//   * "Needs you" — anything unhandled, which is the only part that should
//     ever feel urgent.
//   * "Earlier" — the settled history, calm and countable.
//   * When nothing has ever happened, the canary holds the empty state and
//     says so. A quiet fleet is the product working, and it should read like
//     an achievement rather than a blank screen.

import SwiftUI

struct AlertsView: View {
    @EnvironmentObject var store: FleetStore
    @State private var showingRules = false

    private var unhandled: [AlertRecord] {
        store.alertLog.records.filter { $0.handling == .new }
    }
    private var settled: [AlertRecord] {
        store.alertLog.records.filter { $0.handling != .new }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HeartbeatCard().listRowInsets(EdgeInsets())
                        .listRowBackground(Color.clear)
                }

                if store.alertLog.isQuiet {
                    Section {
                        QuietStateCard(trustDays: store.canaryTrustDays)
                            .listRowInsets(EdgeInsets())
                            .listRowBackground(Color.clear)
                    }
                }

                if !unhandled.isEmpty {
                    Section("Needs you") {
                        ForEach(unhandled) { record in
                            AlertRecordRow(record: record)
                        }
                    }
                }

                if !settled.isEmpty {
                    Section("Earlier") {
                        ForEach(settled) { record in
                            AlertRecordRow(record: record)
                        }
                    }
                }
            }
            .navigationTitle("Alerts")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Tell me when…") { showingRules = true }
                }
            }
            .sheet(isPresented: $showingRules) {
                AlertRulesSheet(center: store.alerts)
            }
        }
    }
}

// MARK: - one thing that happened

struct AlertRecordRow: View {
    @EnvironmentObject var store: FleetStore
    let record: AlertRecord

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            HStack(spacing: Theme.s) {
                Image(systemName: record.severity.sfSymbol)
                    .foregroundStyle(Theme.color(record.severity.role))
                    .accessibilityHidden(true)
                Text(record.name).font(.body.weight(.medium))
                Spacer(minLength: Theme.s)
                if record.count > 1 {
                    // The collapse, made visible: a flapping Canary is one
                    // line that says how often, never sixty lines.
                    Text("\(record.count)×")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            Text(record.headline)
                .font(.subheadline)
                .foregroundStyle(.secondary)
            HStack(spacing: Theme.s) {
                // Coarse by construction — the record only ever knew a
                // 10-minute bucket (Invariant III).
                Text(record.lastBucket, format: .relative(presentation: .named))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                ReachChip(delivery: record.delivery, reason: record.undeliveredReason)
                if record.handling == .acknowledged {
                    Label("Acknowledged", systemImage: "checkmark")
                        .font(.caption2).foregroundStyle(Theme.color(.calm))
                        .labelStyle(.titleAndIcon)
                } else if record.handling == .muted {
                    Label("Muted", systemImage: "bell.slash")
                        .font(.caption2).foregroundStyle(.secondary)
                        .labelStyle(.titleAndIcon)
                }
            }
        }
        .padding(.vertical, 2)
        .swipeActions(edge: .leading, allowsFullSwipe: true) {
            Button {
                store.acknowledgeAlert(for: record.witnessID)
            } label: { Label("Acknowledge", systemImage: "checkmark") }
                .tint(Theme.color(.calm))
        }
        .swipeActions(edge: .trailing) {
            Button {
                store.mute(record.witnessID)
            } label: { Label("Mute 1 hour", systemImage: "bell.slash") }
                .tint(Theme.color(.warn))
        }
    }
}

/// How far this one actually got. The whole point of showing it: "we told you"
/// and "we couldn't tell you" must never look the same.
struct ReachChip: View {
    let delivery: AlertDelivery
    var reason: String?

    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: delivery.sfSymbol)
            Text(delivery == .notDelivered ? (reason ?? delivery.label) : delivery.label)
        }
        .font(.caption2)
        .foregroundStyle(delivery == .notDelivered ? Theme.color(.warn) : .secondary)
        .accessibilityLabel(delivery == .notDelivered
                            ? "Not delivered. \(reason ?? "")"
                            : "Reached you \(delivery.label)")
    }
}

// MARK: - the state we want people to live in

struct QuietStateCard: View {
    var trustDays: Int

    var body: some View {
        VStack(spacing: Theme.s) {
            CanaryActor(face: .calm, height: 64)
            Text("Nothing has needed you.")
                .font(.headline)
            Text(trustDays >= 7
                 ? "\(trustDays) clean days together."
                 : "When something does, it lands here — and you'll know how it reached you.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, Theme.l)
    }
}

// MARK: - "Tell me when…" (the rules, in plain words)

struct AlertRulesSheet: View {
    @ObservedObject var center: AlertCenter
    @ObservedObject private var away = AwayPush.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach($center.rules) { $rule in
                        AlertRuleRow(rule: $rule, awayReach: away.reach)
                    }
                } header: {
                    Text("Tell me when")
                } footer: {
                    Text("We push only what you arm here, and only what matters — like a smoke alarm, silent until it isn't. Everyday activity stays in Today, never a buzz.")
                }

                Section {
                    Label(away.reach.explanation,
                          systemImage: away.reach.isReady
                              ? "antenna.radiowaves.left.and.right"
                              : "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(away.reach.isReady ? .secondary : Theme.color(.warn))
                    if !away.reach.isReady {
                        Button("Set up away alerts") {
                            Task { await away.enable() }
                        }
                    }
                } header: {
                    Text("Away from home")
                } footer: {
                    Text("Away alerts travel through your own iCloud: a device that's home posts a wake carrying nothing but how serious it is — no name, no time, no footage — and your iPhone writes the words itself. SecuraCV runs no server in that path and can't read any of it.")
                }
            }
            .navigationTitle("Alerts")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task { if !away.reach.isReady { await away.enable() } }
        }
    }
}

struct AlertRuleRow: View {
    @Binding var rule: AlertRule
    var awayReach: AwayReach

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            Toggle(isOn: $rule.enabled) {
                Text(rule.title).font(.body)
            }
            Picker("Reach", selection: $rule.reach) {
                Text("On Wi-Fi only").tag(AlertRule.Reach.onWiFiOnly)
                Text("Anywhere").tag(AlertRule.Reach.anywhere)
            }
            .pickerStyle(.segmented)
            .disabled(!rule.enabled)
            // Honesty about reach is the feature: if the away path can't
            // carry anything right now, the rule says so instead of leaving
            // "Anywhere" looking like a working promise.
            if rule.reach == .anywhere, rule.enabled, !awayReach.isReady {
                Label("Can't reach you away yet — \(awayReach.explanation)",
                      systemImage: "exclamationmark.triangle")
                    .font(.caption2)
                    .foregroundStyle(Theme.color(.warn))
            }
        }
        .padding(.vertical, 2)
    }
}

#Preview("Alerts — demo fleet") {
    AlertsView().environmentObject(DemoFleet.previewStore())
}
