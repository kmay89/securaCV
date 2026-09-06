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
    @EnvironmentObject var store: WristStore

    /// The freshest row for this witness — the pushed value can go stale the
    /// moment a mute reply lands, so render the snapshot's copy when it has
    /// one.
    private var live: WristWitness {
        store.snapshot?.witnesses.first { $0.id == witness.id } ?? witness
    }

    var body: some View {
        List {
            Section {
                HStack(spacing: Theme.s) {
                    SeverityPip(severity: live.severity)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(live.name).font(.headline)
                        Text(live.severity.label)
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }

            Section {
                LabeledContent("Link", value: live.link.label)
                LabeledContent {
                    Text(live.badge.label)
                } label: {
                    Label {
                        Text("Chain")
                    } icon: {
                        Image(systemName: live.badge.sfSymbol)
                            .foregroundStyle(live.badge.isTrusted
                                             ? Theme.color(.calm) : .secondary)
                    }
                }
                if live.tamper {
                    Label(live.tamperHeadline, systemImage: "hand.raised.slash.fill")
                        .foregroundStyle(Theme.color(.tamper))
                }
                if let battery = live.batteryPct {
                    LabeledContent("Battery", value: "\(battery)%")
                }
            }

            // "Where IS it?" from the wrist's own radio — offered whenever
            // the beacon can be recognized (an older phone sends no
            // fingerprint and the row simply doesn't offer it) and the
            // phone's consent-first discovery choice says yes. The screen
            // itself carries the honest gates for consent and Bluetooth.
            if live.fingerprint != nil {
                Section {
                    NavigationLink {
                        WristFindView(witness: live)
                    } label: {
                        Label("Find", systemImage: "location.north.circle")
                    }
                } footer: {
                    Text("Warmer/colder by its beacon — taps guide your hand; a WAP-class Canary can chirp back.")
                }
            }

            if let event = live.lastEventHeadline {
                Section("Last event") {
                    VStack(alignment: .leading, spacing: Theme.xs) {
                        Text(event).font(.body)
                        if let bucket = live.lastEventBucket {
                            Text("≈ ") + Text(bucket, style: .relative) + Text(" ago")
                        }
                    }
                    .font(.caption2)
                }
            }

            Section {
                if live.isMuted {
                    Label("Muted", systemImage: "bell.slash.fill")
                        .foregroundStyle(.secondary)
                } else if store.isPhoneReachable {
                    // The same three lengths the phone offers, filtered by
                    // the same rule (MuteDuration.offered) — one definition
                    // of "until tonight" for both wrists and pockets.
                    ForEach(MuteDuration.offered(at: Date())) { duration in
                        Button {
                            store.mute(id: live.id, duration: duration)
                        } label: {
                            Label(duration.title, systemImage: duration.sfSymbol)
                        }
                    }
                }
            } footer: {
                Text("Muting quiets the nagging, never the truth — tamper and a failed signature still punch through. Every mute ends on its own; unmute early from your iPhone.")
            }
        }
        .navigationTitle(live.name)
    }
}

#Preview("Witness detail — sample") {
    NavigationStack {
        WitnessDetailView(witness: WristSnapshot.sample().witnesses[0])
            .environmentObject(WristStore.preview())
    }
}
