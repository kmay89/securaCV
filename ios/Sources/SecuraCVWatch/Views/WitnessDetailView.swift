// WitnessDetailView.swift  (watch app target)
//
// One Canary, one screen deep — the glanceable facts only. Event times are
// the coarse 10-minute buckets the snapshot carries (Invariant III), shown
// with a "≈" so the coarseness reads as a promise, not imprecision. Mute is
// ASKED for here, never decided here: the wrist sends WristSync.commandMute
// with one of the phone's own lengths, and the phone owns how mute behaves
// (its ledger, the tamper punch-through, what "tonight" means); the reply
// snapshot shows the result.
//
// What the device is and how its room stands mirror the phone's detail
// screen and the Wall's card through the shared resolvers (DeviceNaming,
// FleetFigureBridge, DeviceGlanceCopy) — mirrored, not cloned: one product
// line, one room line, one hub line, and nothing a glance can't use.

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
                    DeviceFigureIcon(live.deviceType,
                                     published: live.publishedType,
                                     hardware: live.hardware,
                                     size: 30)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(live.name).font(.headline)
                        HStack(spacing: Theme.xs) {
                            SeverityPip(severity: live.severity)
                            Text(live.severity.label)
                        }
                        .font(.caption2).foregroundStyle(.secondary)
                        // The product NAME, never the wire string; nothing
                        // at all for a type this build doesn't know.
                        if let product = DeviceNaming.productName(published: live.publishedType,
                                                                  hardware: live.hardware) {
                            Text(product)
                                .font(.caption2).foregroundStyle(.secondary)
                        }
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
                // Only when there is something to do about it — a
                // connected hub, or a device that never said, draws nothing.
                if let hubLine = DeviceGlanceCopy.hubLine(live.hub) {
                    Label(hubLine, systemImage: "server.rack")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }

            // The radar's coarse room words, when this row carries any — the
            // same sentence the Wall's card says. Absence draws nothing:
            // "cannot say" is never rendered as an empty, calm room.
            if let room = DeviceGlanceCopy.wellbeingLine(present: live.radarPresent,
                                                         occupants: live.radarOccupants,
                                                         breathing: live.breathingLock) {
                Section {
                    Text(room).font(.caption2)
                } header: {
                    Text("Room")
                } footer: {
                    Text("As the radar last reported it. Coarse by design — no camera, no identity.")
                }
            }

            // "Where IS it?" from the wrist's own radio — offered whenever
            // the beacon can be recognized (an older phone sends no
            // fingerprint and the row simply doesn't offer it) and the
            // phone's consent-first discovery choice says yes. The screen
            // itself carries the honest gates for consent and Bluetooth.
            if live.fingerprint != nil {
                Section {
                    // Value-based, so this link and the complication's deep
                    // link travel through the one registered destination.
                    NavigationLink(value: WristFindRoute(witness: live)) {
                        Label("Find", systemImage: "location.north.circle")
                    }
                } footer: {
                    // Chirp is promised only for a row the phone said can
                    // answer it (a paired WAP) — the same gate as the button.
                    Text(live.canIdentify == true
                         ? "Warmer/colder by its beacon — taps guide your hand, and this one can chirp back."
                         : "Warmer/colder by its beacon — taps guide your hand.")
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
