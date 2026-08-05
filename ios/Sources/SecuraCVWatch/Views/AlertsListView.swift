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
// (`WristSync.maxAlertRows`), acknowledgment travels back over the existing
// mute/ack vocabulary, and nothing here invents state the phone didn't send.

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
                            WristAlertRow(row: row)
                        }
                    }
                }
            }
            .navigationTitle("Alerts")
        }
    }
}

struct WristAlertRow: View {
    @EnvironmentObject var store: WristStore
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
