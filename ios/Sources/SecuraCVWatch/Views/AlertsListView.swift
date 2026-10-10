// AlertsListView.swift
//
// "What needed me?" on the wrist. The glance answers how the fleet is RIGHT
// NOW; this answers what happened while you weren't looking — the question a
// watch is actually good at, because it's the screen you check on a walk.
//
// Same honesty as the phone: each row says how far the alert got. A row that
// says "Not delivered" is the most important thing this app can show, and it
// must never look like a delivered one.
//
// The wrist stays a glance: the phone caps the rows it sends
// (`WristSync.maxAlertRows`), Acknowledge and Mute travel back as phone
// verbs (`WristSync.commandAck` / `commandMute`) — the phone's one
// acknowledgment clears the alert everywhere (RFC §1.1 rule 4) — and nothing
// here invents state the phone didn't send. A row whose Canary is in the
// snapshot opens that Canary's detail.

import SwiftUI

struct AlertsListView: View {
    @EnvironmentObject var store: WristStore

    private var rows: [WristAlert] { store.snapshot?.alerts ?? [] }

    var body: some View {
        NavigationStack {
            Group {
                if rows.isEmpty {
                    QuietWristState(face: store.snapshot?.face ?? .calm,
                                    trustDays: store.snapshot?.trustDays ?? 0)
                } else {
                    List {
                        ForEach(rows) { row in
                            Group {
                                if let witness = store.snapshot?.witnesses.first(where: { $0.id == row.witnessID }) {
                                    NavigationLink(value: witness) { WristAlertRow(row: row) }
                                } else {
                                    WristAlertRow(row: row)
                                }
                            }
                            // On the ROW (the link itself when there is one),
                            // where a List looks for swipe actions.
                            .modifier(WristAlertActions(row: row))
                        }
                    }
                    // The same two destinations the glance registers, so the
                    // detail's Find link works from here too.
                    .navigationDestination(for: WristWitness.self) { WitnessDetailView(witness: $0) }
                    .navigationDestination(for: WristFindRoute.self) { WristFindView(witness: $0.witness) }
                }
            }
            .navigationTitle("Alerts")
        }
    }
}

struct WristAlertRow: View {
    let row: WristAlert

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: Theme.xs) {
                Image(systemName: row.severity.sfSymbol)
                    .foregroundStyle(Theme.color(row.severity.role))
                    .accessibilityHidden(true)
                Text(row.name).font(.headline).lineLimit(1)
                if row.count > 1 {
                    Text("\(row.count)×")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            Text(row.headline)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
            HStack(spacing: Theme.xs) {
                Text(row.bucket, format: .relative(presentation: .named))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                if row.resolved == true {
                    // "Over" must never look like "still happening" — the
                    // same rule as the phone. nil (an older phone) claims
                    // nothing either way.
                    Label("Cleared", systemImage: "checkmark.circle")
                        .font(.caption2)
                        .foregroundStyle(Theme.color(.calm))
                        .labelStyle(.titleAndIcon)
                }
                if row.delivery == .notDelivered {
                    Label("Not delivered", systemImage: "bell.slash")
                        .font(.caption2)
                        .foregroundStyle(Theme.color(.warn))
                        .labelStyle(.titleAndIcon)
                } else if row.handling == .acknowledged {
                    Image(systemName: "checkmark")
                        .font(.caption2)
                        .foregroundStyle(Theme.color(.calm))
                        .accessibilityLabel("Acknowledged")
                } else if row.handling == .muted {
                    Image(systemName: "bell.slash")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("Muted")
                }
            }
        }
        .padding(.vertical, 2)
    }
}

/// Acknowledge (leading) and Mute (trailing) for one alert row — phone verbs,
/// carried out by the phone (WristSync.commandAck / commandMute).
struct WristAlertActions: ViewModifier {
    @EnvironmentObject var store: WristStore
    let row: WristAlert

    func body(content: Content) -> some View {
        content
            // The acknowledgment the phone, its notifications and the display
            // already offer — only for a row that still wants a human, and only
            // while the phone can carry it out.
            .swipeActions(edge: .leading) {
                if row.handling == .new, row.resolved != true, store.isPhoneReachable {
                    Button {
                        store.acknowledge(witnessID: row.witnessID)
                    } label: { Label("Acknowledge", systemImage: "checkmark") }
                        .tint(Theme.color(.calm))
                }
            }
            .swipeActions(edge: .trailing) {
                Button {
                    store.mute(id: row.witnessID)
                } label: { Label("Mute", systemImage: "bell.slash") }
                    .tint(Theme.color(.warn))
            }
    }
}

/// The empty state the product is trying to earn.
struct QuietWristState: View {
    var face: CanaryFace
    var trustDays: Int

    var body: some View {
        VStack(spacing: Theme.s) {
            if face != .hidden {
                CanaryActor(face: face, height: 40)
            }
            Text("Nothing needed you.")
                .font(.headline)
                .multilineTextAlignment(.center)
            if trustDays >= 7 {
                Text("\(trustDays) clean days together.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding()
    }
}
