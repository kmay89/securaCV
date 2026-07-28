// AlertsView.swift
//
// Not a settings dump — a plain-language "tell me when…" list. Each rule shows
// how far it can reach you (on Wi-Fi only, or anywhere via the opt-in relay).
// Honesty about reach is the feature. The push discipline lives in AlertCenter;
// this is its face.

import SwiftUI

struct AlertsView: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HeartbeatCard().listRowInsets(EdgeInsets())
                        .listRowBackground(Color.clear)
                }
                Section("Tell me when") {
                    // Bindings must come from the AlertCenter directly, so the
                    // rules editor observes it rather than reaching through the
                    // store (you can't bind into a nested ObservableObject).
                    AlertRulesEditor(center: store.alerts)
                }
                Section {
                    if !store.alerts.authorized {
                        Label("Turn on notifications so alerts can reach you.",
                              systemImage: "bell.badge")
                            .foregroundStyle(Theme.color(.warn))
                    }
                } footer: {
                    Text("We push only what you arm here, and only what matters — like a smoke alarm, silent until it isn't. Everyday activity stays in Today, never a buzz.")
                }
            }
            .navigationTitle("Alerts")
        }
    }
}

/// Observes the AlertCenter so it can hand out real Bindings to each rule.
struct AlertRulesEditor: View {
    @ObservedObject var center: AlertCenter
    var body: some View {
        ForEach($center.rules) { $rule in
            AlertRuleRow(rule: $rule)
        }
    }
}

struct AlertRuleRow: View {
    @Binding var rule: AlertRule
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
            if rule.reach == .anywhere {
                Text("Away alerts use the metadata-only relay — a wake token, never footage.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}

#Preview("Alerts — demo fleet") {
    AlertsView().environmentObject(DemoFleet.previewStore())
}
