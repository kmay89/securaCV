// WitnessDetailView.swift  (watch app target)
//
// One Canary, one screen deep — the glanceable facts only. Event times are
// the coarse 10-minute buckets the snapshot carries (Invariant III), shown
// with a "≈" so the coarseness reads as a promise, not imprecision. Mute is
// SHOWN here but not changed here: per-witness mute semantics live in
// fleet_model.h and the phone surfaces them; the wrist never reinvents them
// (RFC §3.4 — settings are phone territory).

import SwiftUI

struct WitnessDetailView: View {
    let witness: WristWitness

    var body: some View {
        List {
            Section {
                HStack(spacing: Theme.s) {
                    SeverityPip(severity: witness.severity)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(witness.name).font(.headline)
                        Text(witness.severity.label)
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }

            Section {
                LabeledContent("Link", value: witness.link.label)
                LabeledContent {
                    Text(witness.badge.label)
                } label: {
                    Label {
                        Text("Chain")
                    } icon: {
                        Image(systemName: witness.badge.sfSymbol)
                            .foregroundStyle(witness.badge.isTrusted
                                             ? Theme.color(.calm) : .secondary)
                    }
                }
                if witness.tamper {
                    Label("Tamper detected", systemImage: "hand.raised.slash.fill")
                        .foregroundStyle(Theme.color(.tamper))
                }
                if let battery = witness.batteryPct {
                    LabeledContent("Battery", value: "\(battery)%")
                }
                if witness.isMuted {
                    Label("Muted from your iPhone", systemImage: "bell.slash.fill")
                        .foregroundStyle(.secondary)
                }
            }

            if let event = witness.lastEventHeadline {
                Section("Last event") {
                    VStack(alignment: .leading, spacing: Theme.xs) {
                        Text(event).font(.body)
                        if let bucket = witness.lastEventBucket {
                            Text("≈ ") + Text(bucket, style: .relative) + Text(" ago")
                        }
                    }
                    .font(.caption2)
                }
            }
        }
        .navigationTitle(witness.name)
    }
}

#Preview("Witness detail — sample") {
    NavigationStack {
        WitnessDetailView(witness: WristSnapshot.sample().witnesses[0])
    }
}
