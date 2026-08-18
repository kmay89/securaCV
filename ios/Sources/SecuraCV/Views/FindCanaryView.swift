// FindCanaryView.swift
//
// "Find this Canary" — the hot/cold search, built on what the fleet really
// broadcasts. Every Canary's presence beacon arrives several times a second
// with a signal level; ProximityRanger (pure, host-tested) smooths it into
// honest BANDS and a warmer/colder trend, and this screen walks you in:
//
//   * the ring tightens as you close in; the words stay hedged ("about a
//     room away") because RSSI is bands, not meters — there is no
//     ultra-wideband radio on a Canary, so there is honestly no arrow;
//   * the hand is guided too: one graded tap per band crossed on the way
//     in, one success tap on arrival, silence while nothing changes
//     (ProximityRanger.tick — the FeedbackPolicy discipline, applied to
//     the one screen where the user asked to be led);
//   * a WAP-class Canary can answer back: "Make it chirp" asks the device
//     itself to blink and chirp for ~15 s (/api/identify) — the part of
//     Find My even AirTags do, done by the witness itself;
//   * with several Canaries around, the screen says so honestly: "You're
//     closer to Kitchen right now" — the beacon suffix ties each signal to
//     its named row, so a fleet is a set of named signals, not a pile.

import SwiftUI

struct FindCanaryView: View {
    let witness: Witness
    @EnvironmentObject var store: FleetStore
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @State private var ranger = ProximityRanger()
    @State private var reading = ProximityRanger.Reading(band: .searching,
                                                         trend: .unknown,
                                                         smoothedDBM: nil)
    @State private var neighborHint: String?
    @State private var chirpUntil: Date?
    @State private var chirpNote: String?

    var body: some View {
        Group {
            if store.discoveryConsent == true {
                findingBody
            } else {
                // The radios only run with the user's consent (the same gate
                // the Fleet tab honors) — an endless "Listening…" over a
                // radio that is off would be this screen lying.
                ContentUnavailableView {
                    Label("Finding needs discovery", systemImage: "dot.radiowaves.left.and.right")
                } description: {
                    Text("Finding follows this Canary's Bluetooth beacon, and the radios are off until you enable discovery.")
                } actions: {
                    Button("Enable discovery") { store.setDiscoveryConsent(true) }
                        .buttonStyle(.borderedProminent)
                }
            }
        }
        .navigationTitle("Find \(witness.displayName)")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var findingBody: some View {
        VStack(spacing: Theme.l) {
            Spacer(minLength: 0)

            // The finding ring: the device you're looking for in the middle
            // (its real figure), a ring that tightens band by band.
            ZStack {
                Circle()
                    .stroke(Theme.color(.info).opacity(0.15), lineWidth: 10)
                Circle()
                    .trim(from: 0, to: reading.band.ringFraction)
                    .stroke(ringColor, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(reduceMotion ? nil : .easeInOut(duration: 0.6),
                               value: reading.band)
                DeviceFigureIcon(witness.deviceType, published: witness.publishedType,
                                 hardware: witness.hardware, size: 84)
            }
            .frame(width: 220, height: 220)
            .accessibilityHidden(true)

            VStack(spacing: Theme.s) {
                Text(reading.band.label)
                    .font(.title.bold())
                    .contentTransition(.opacity)
                if let trend = reading.trend.label {
                    Text(trend)
                        .font(.headline)
                        .foregroundStyle(reading.trend == .warmer
                                         ? Theme.color(.calm) : Theme.color(.warn))
                }
                Text(reading.band.hint)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                if let neighborHint {
                    Text("You're closer to \(neighborHint) right now.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal)

            Spacer(minLength: 0)

            if canIdentify {
                VStack(spacing: Theme.s) {
                    Button {
                        Task { await chirp() }
                    } label: {
                        Label(chirpButtonTitle, systemImage: "speaker.wave.2.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(chirpUntil.map { $0 > Date() } ?? false)
                    if let chirpNote {
                        Text(chirpNote)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                }
                .padding(.horizontal)
            }

            Text("Guided by this Canary's own Bluetooth beacon — room-level, not an exact arrow. Walls and doors move the signal more than a step does.")
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding([.horizontal, .bottom])
        }
        .task { await run() }
    }

    // MARK: - the search loop

    /// Twice a second: feed the freshest matching advert into the ranger,
    /// let staleness speak when there is none, tick the hand only on band
    /// transitions, and refresh the closer-neighbor hint.
    private func run() async {
        while !Task.isCancelled {
            let before = reading.band
            if let sighting = strongestMatch() {
                reading = ranger.ingest(rssiDBM: sighting.rssiDBM, at: Date())
            } else {
                reading = ranger.reading(at: Date())
            }
            Feedback.play(finding: ProximityRanger.tick(from: before, to: reading.band))
            neighborHint = ProximityRanger.nearerNeighbor(
                targetDBM: ranger.smoothedDBM, neighbors: namedNeighbors())
            try? await Task.sleep(for: .milliseconds(500))
        }
    }

    /// The freshest, strongest beacon whose fingerprint suffix matches this
    /// witness. The suffix narrows rather than proves (two bytes), which is
    /// exactly right for a finding hint — pairing and the chain remain the
    /// real identity gates.
    private func strongestMatch() -> BeaconSighting? {
        guard !witness.fingerprint.isEmpty else { return nil }
        return store.ble.freshSightings
            .filter { $0.beacon.matches(fingerprint: witness.fingerprint) }
            .max { $0.rssiDBM < $1.rssiDBM }
    }

    /// Every OTHER fleet member currently heard, by its real name — the
    /// input to "you're closer to X".
    private func namedNeighbors() -> [(name: String, dbm: Double)] {
        store.ble.freshSightings.compactMap { sighting in
            guard let other = store.witnesses.first(where: {
                !$0.fingerprint.isEmpty && $0.id != witness.id
                    && sighting.beacon.matches(fingerprint: $0.fingerprint)
            }) else { return nil }
            return (other.displayName, Double(sighting.rssiDBM))
        }
    }

    // MARK: - make it answer back

    /// Only a paired WAP-class Canary serves /api/identify.
    private var canIdentify: Bool {
        witness.deviceType == .wap
            && store.devices.devices.contains { $0.id == witness.id && $0.baseURL != nil }
    }

    private var chirpButtonTitle: String {
        if let until = chirpUntil, until > Date() { return "Chirping…" }
        return "Make it chirp"
    }

    private func chirp() async {
        guard let ref = store.devices.devices.first(where: { $0.id == witness.id }),
              let api = try? store.devices.api(for: ref) else { return }
        do {
            let visualOnly = try await api.identify()
            chirpUntil = Date().addingTimeInterval(15)
            chirpNote = visualOnly
                ? "It's blinking — this Canary's chirp is set to visual-only."
                : "Listen for the chirp and watch for the blink — about 15 seconds."
        } catch {
            // Honest reach: the ask travels over Wi-Fi, so a Canary you can
            // hear on Bluetooth may still be out of HTTP reach.
            chirpNote = "Couldn't reach it over Wi-Fi to ask — \(error.localizedDescription)"
        }
    }

    private var ringColor: Color {
        switch reading.band {
        case .here: return Theme.color(.calm)
        case .veryClose, .near: return Theme.color(.info)
        default: return Theme.color(.info).opacity(0.6)
        }
    }
}

#Preview("Find — demo Canary") {
    NavigationStack {
        FindCanaryView(witness: DemoFleet.witnesses().first!)
            .environmentObject(DemoFleet.previewStore())
    }
}
