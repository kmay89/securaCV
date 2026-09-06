// WristFindView.swift  (watch app target)
//
// "Find this Canary", on the arm doing the walking. The wrist's own radio
// (WristFinder) hears the fleet's beacons directly — no phone required in
// range — and the same host-tested arithmetic as the phone's Find screen
// (Shared/ProximityRanger) turns them into honest bands, a warmer/colder
// trend, and the graded taps that make a hot/cold search work with the
// screen barely glanced at. The taps ARE the interface here; the glass
// mostly confirms what the wrist already felt.
//
// The same honesty rules as the phone, because they are the same policy:
// hedged words (bands, never meters — there is no arrow to draw), a quiet
// beacon reads "Listening…" within seconds, twins that share a beacon
// suffix refuse the signal and say so, and the radio runs only behind the
// phone's consent-first discovery choice, which travels in the snapshot.
//
// "Chirp" relays through the phone (identify travels over Wi-Fi by device
// id) — offered when the phone is reachable, honest about why when not.

import SwiftUI

struct WristFindView: View {
    let witness: WristWitness
    @EnvironmentObject var store: WristStore
    @StateObject private var finder = WristFinder()

    @State private var chirpNote: String?
    @State private var chirpInFlight = false

    private var consented: Bool {
        store.snapshot?.discoveryConsented == true
    }

    /// The same twin rule as the phone's Find screen — one shared,
    /// host-tested definition (ProximityRanger.isSuffixAmbiguous).
    private var suffixIsAmbiguous: Bool {
        guard let fingerprint = witness.fingerprint else { return false }
        let all = (store.snapshot?.witnesses ?? []).compactMap(\.fingerprint)
        return ProximityRanger.isSuffixAmbiguous(fingerprint: fingerprint, among: all)
    }

    var body: some View {
        Group {
            if !consented {
                // The phone's discovery gate travels with the data; the
                // wrist never scans for a user who said no (or was never
                // asked) there.
                VStack(spacing: Theme.s) {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                    Text("Finding needs discovery — turn it on in SecuraCV on your iPhone.")
                        .font(.footnote)
                        .multilineTextAlignment(.center)
                }
            } else if finder.bluetoothDenied {
                Text("Bluetooth is off for SecuraCV — allow it in Settings on this watch.")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
            } else {
                findingBody
            }
        }
        .navigationTitle(witness.name)
        // Keyed on consent so a refresh that carries a new answer from the
        // phone starts (or never starts) the radio accordingly.
        .task(id: consented) {
            guard consented, let fingerprint = witness.fingerprint else { return }
            let neighbors = (store.snapshot?.witnesses ?? []).compactMap {
                row -> (name: String, fingerprint: String)? in
                guard row.id != witness.id, let fp = row.fingerprint else { return nil }
                return (row.name, fp)
            }
            finder.start(targetFingerprint: fingerprint, neighbors: neighbors)
        }
        .onDisappear { finder.stop() }
    }

    private var findingBody: some View {
        ScrollView {
            VStack(spacing: Theme.s) {
                ZStack {
                    Circle()
                        .stroke(Theme.color(.info).opacity(0.15), lineWidth: 7)
                    Circle()
                        .trim(from: 0, to: finder.reading.band.ringFraction)
                        .stroke(ringColor, style: StrokeStyle(lineWidth: 7, lineCap: .round))
                        .rotationEffect(.degrees(-90))
                        .animation(.easeInOut(duration: 0.6), value: finder.reading.band)
                    Image(systemName: "bird")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                }
                .frame(width: 84, height: 84)
                .accessibilityHidden(true)

                Text(finder.reading.band.label)
                    .font(.headline)
                if let trend = finder.reading.trend.label {
                    Text(trend)
                        .font(.footnote.bold())
                        .foregroundStyle(finder.reading.trend == .warmer
                                         ? Theme.color(.calm) : Theme.color(.warn))
                }
                if suffixIsAmbiguous {
                    Text("Two Canaries share this beacon id — the signal can't tell them apart. Chirp still reaches this one.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                } else {
                    Text(finder.reading.band.hint)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                if let neighbor = finder.nearerNeighbor {
                    Text("Closer to \(neighbor) right now.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if store.isPhoneReachable {
                    Button {
                        chirp()
                    } label: {
                        Label(chirpInFlight ? "Asking…" : "Chirp",
                              systemImage: "speaker.wave.2.fill")
                    }
                    .disabled(chirpInFlight)
                }
                if let chirpNote {
                    Text(chirpNote)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
            .padding(.horizontal, Theme.xs)
        }
    }

    /// Relay the chirp through the phone — identify travels over Wi-Fi by
    /// device id, and the reply says exactly what happened.
    private func chirp() {
        chirpInFlight = true
        chirpNote = nil
        store.identify(id: witness.id) { ok, visualOnly, why in
            chirpInFlight = false
            if ok {
                chirpNote = visualOnly
                    ? "It's blinking (chirp set to visual-only)."
                    : "Listen for it — about 15 seconds."
            } else {
                chirpNote = why ?? "The iPhone couldn't reach it."
            }
        }
    }

    private var ringColor: Color {
        switch finder.reading.band {
        case .here: return Theme.color(.calm)
        case .veryClose, .near: return Theme.color(.info)
        default: return Theme.color(.info).opacity(0.6)
        }
    }
}
