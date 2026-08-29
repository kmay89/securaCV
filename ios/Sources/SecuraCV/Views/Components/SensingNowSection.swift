// SensingNowSection.swift
//
// The room RIGHT NOW, from the WAP's own 1 Hz sensing snapshot
// (GET /api/csi/stream — Wire/WapEvents.swift documents the three variant
// bodies this must render). The device's dashboard has always had this live
// view; the phone showed only the record. This section is the phone's half,
// speaking the dashboard's exact vocabulary so the two surfaces tell one
// story about the same second.
//
// Honesty rules, in order of the ways this tile could lie:
//   * a starved radio must never read as a calm room — the supply health
//     (fps / silent_ms) gates the claim, same as the dashboard;
//   * a vital sign is shown only at the device's own "confirmed" bar;
//   * a failed poll keeps the last real reading briefly rather than
//     flashing errors, but three straight misses say "not reachable" —
//     stale must never quietly impersonate live.

import SwiftUI

struct SensingNowSection: View {
    /// The paired device's authenticated client — resolved by the parent
    /// (DeviceDetailView) with the same guard verifyNow uses, so an
    /// unpaired row or a Keychain-lost token never mounts this section
    /// (the stream is Bearer-gated; it would only 401 forever).
    let api: DeviceAPI

    @State private var snapshot: WapStream?
    @State private var misses = 0

    private var unreachable: Bool { misses >= 3 }

    var body: some View {
        Section {
            if unreachable {
                Text("Not reachable right now.")
                    .foregroundStyle(.secondary)
            } else if let s = snapshot {
                if s.isUnavailable {
                    // The dashboard's exact sentence — one story, every surface.
                    Text("Sensing is offline. The canary's radio is not running.")
                        .foregroundStyle(Theme.color(.warn))
                } else if s.radioSilent {
                    // Starvation gates the CLAIM, not just a footnote under
                    // it: the snapshot's state can be arbitrarily stale when
                    // no frames arrive, so nothing cached may wear "Right
                    // now" — the dashboard parks on "Sensing…" here for the
                    // same reason (review on this tile's first cut).
                    LabeledContent("Right now") {
                        Label("Sensing…", systemImage: "dot.radiowaves.left.and.right")
                            .foregroundStyle(.secondary)
                    }
                    Label("No Wi-Fi signal to sense with — no frames are reaching the radio.",
                          systemImage: "antenna.radiowaves.left.and.right.slash")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } else {
                    LabeledContent("Right now") {
                        Label(s.stateLabel, systemImage: "dot.radiowaves.left.and.right")
                            .foregroundStyle(Theme.color(.info))
                    }
                    if let m = s.motion { LabeledContent("Motion", value: "\(m)%") }
                    if let b = s.breathing { LabeledContent("Breathing", value: "\(b)%") }
                    if let bpm = s.confirmedBPM {
                        LabeledContent("Breaths per minute", value: "\(bpm)")
                    }
                }
            } else {
                HStack {
                    Text("Listening for the room")
                    Spacer()
                    ProgressView()
                }
                .foregroundStyle(.secondary)
            }
        } header: {
            Text("Sensing now")
        } footer: {
            Text("Live over your own Wi-Fi — a 1-second poll while this screen is open. Nothing leaves the network, and nothing is stored.")
        }
        // FindCanaryView's while-visible idiom: SwiftUI cancels this task
        // the moment the screen goes away, so the poll can never outlive
        // the reader's attention or run in the background.
        .task { await poll() }
    }

    private func poll() async {
        while !Task.isCancelled {
            if let s = try? await api.wapStream() {
                snapshot = s
                misses = 0
            } else {
                misses += 1
            }
            try? await Task.sleep(for: .seconds(1))
        }
    }
}
