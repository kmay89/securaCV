// FleetGlanceView.swift  (watch app target)
//
// RFC §3.3 screen 1: the worst-first fleet list under a one-line roll-up.
// Every honesty cue travels with the data: the demo banner when the state is
// sample data, the "as of" staleness line, the omitted-rows count when the
// snapshot capped the list, and a truthful empty state instead of a spinner
// that implies work is happening.

import SwiftUI

struct FleetGlanceView: View {
    @EnvironmentObject var store: WristStore

    var body: some View {
        NavigationStack {
            Group {
                if let snap = store.snapshot {
                    fleetList(snap)
                } else {
                    ContentUnavailableView {
                        Label {
                            Text("No fleet yet")
                        } icon: {
                            // The character at rest — the first thing a new
                            // user sees breathes, gently, like ours.
                            CanaryActor(face: .calm, height: 36)
                        }
                    } description: {
                        Text("Open SecuraCV on your iPhone — your fleet appears here on its own.")
                    }
                }
            }
            .navigationTitle("Fleet")
        }
    }

    private func fleetList(_ snap: WristSnapshot) -> some View {
        List {
            if store.phoneSpeaksNewerSchema {
                Label("Update this watch app to match your iPhone.",
                      systemImage: "exclamationmark.arrow.triangle.2.circlepath")
                    .font(.caption2)
                    .foregroundStyle(Theme.color(.warn))
            }
            if snap.isDemoData {
                Label("Sample data — pair a Canary from your iPhone.",
                      systemImage: "sparkles")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            Section {
                VStack(alignment: .leading, spacing: Theme.xs) {
                    HStack(spacing: Theme.s) {
                        // The character IS the status while it may be: the
                        // mood engine's face (same engine as the bedside
                        // glass). During a real unacknowledged alarm the
                        // face is .hidden and the instrument takes the
                        // stage — never cute during a real alarm.
                        if snap.face == .hidden {
                            SeverityPip(severity: snap.severity)
                        } else {
                            CanaryActor(face: snap.face, posture: snap.posture, height: 34)
                        }
                        Text(snap.headline).font(.headline).lineLimit(2)
                    }
                    Text("\(snap.healthy)/\(snap.total) healthy")
                        .font(.caption2).foregroundStyle(.secondary)
                        .monospacedDigit()
                    if let line = snap.moodLine {
                        Text(line)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
            }

            Section {
                ForEach(snap.witnesses) { w in
                    NavigationLink(value: w) { WitnessGlanceRow(witness: w) }
                }
                if snap.omittedWitnesses > 0 {
                    Text("+ \(snap.omittedWitnesses) more on your iPhone")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            } footer: {
                staleness(snap)
            }
        }
        .navigationDestination(for: WristWitness.self) { WitnessDetailView(witness: $0) }
    }

    private func staleness(_ snap: WristSnapshot) -> some View {
        // sentAt is when the PHONE composed this truth — the honest age.
        HStack(spacing: Theme.xs) {
            Image(systemName: store.isPhoneReachable ? "iphone" : "iphone.slash")
            Text("As of ") + Text(snap.sentAt, style: .relative) + Text(" ago")
        }
        .font(.caption2)
        .foregroundStyle(.secondary)
    }
}

struct WitnessGlanceRow: View {
    let witness: WristWitness

    var body: some View {
        HStack(spacing: Theme.s) {
            SeverityPip(severity: witness.severity)
            VStack(alignment: .leading, spacing: 0) {
                Text(witness.name).font(.body).lineLimit(1)
                Text(subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 0)
            if witness.isMuted {
                Image(systemName: "bell.slash.fill")
                    .font(.caption2).foregroundStyle(.secondary)
                    .accessibilityLabel("Muted")
            }
        }
    }

    private var subtitle: String {
        if witness.tamper { return witness.tamperHeadline }
        if witness.badge == .failed { return "Signature did not verify" }
        if witness.link.isDark { return "Gone dark" }
        if let event = witness.lastEventHeadline { return event }
        return witness.link.label
    }
}

#Preview("Fleet glance — sample") {
    FleetGlanceView().environmentObject(WristStore.preview())
}
