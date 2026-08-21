// DeviceDetailView.swift
//
// One Canary up close: trust (chain length + badge + a "Verify now" that
// re-checks Ed25519 on the phone), liveness/diagnostics, mute, and a link to
// two-way talk / AirPlay chime where the hardware supports it. Config is
// rendered from the device's OWN schema, so new firmware sections appear here
// with no app update.

import SwiftUI

struct DeviceDetailView: View {
    let witness: Witness
    @EnvironmentObject var store: FleetStore
    @State private var verifying = false
    @State private var verdict: ChainVerdict?
    @State private var showingGlassSettings = false

    /// The pushed `witness` value goes stale the moment the store updates
    /// (e.g. right after Mute) — render mute state from the live row.
    private var liveWitness: Witness {
        store.witnesses.first { $0.id == witness.id } ?? witness
    }

    @ObservedObject private var home = HomeKitBridge.shared

    private var standing: HomeKitStanding {
        home.standing(for: liveWitness,
                      presentInHome: home.seenInHome(liveWitness),
                      homeHubPresent: home.homeHubPresent)
    }

    /// Where the nightlight answers HTTP: a paired base URL if one exists,
    /// else the mDNS host discovery heard it advertise, else the host baked
    /// into a `lan:<host>#<index>` provisional id (the /api/fleet rows —
    /// those never match a discovery row by id, so the id IS the route).
    /// Hosts go through the same normalization every discovered address
    /// takes (`DeviceAPI.url(forDiscoveredHost:)`) so a bare mDNS label
    /// gains its `.local` and passes the private-address gate. Nil when no
    /// route exists — the section says "not reachable" instead of guessing.
    private var nightlightBaseURL: URL? {
        if let url = liveWitness.baseURL { return url }
        if let host = store.discovery.found.first(where: { $0.deviceID == witness.id })?.host {
            return DeviceAPI.url(forDiscoveredHost: host)
        }
        if witness.id.hasPrefix("lan:") {
            let host = witness.id.dropFirst(4).split(separator: "#").first.map(String.init) ?? ""
            return DeviceAPI.url(forDiscoveredHost: host)
        }
        return nil
    }

    private var standingLabel: String {
        switch standing {
        case .off: return "Off"
        case .needsAuthorization: return "Needs access"
        case .notPaired: return "Not seen"
        case .paired: return "In the home"
        case .pairedWithoutHomeHub: return "In the home, no hub"
        case .staleInHome: return "Stale"
        }
    }

    var body: some View {
        List {
            // The device itself, before any numbers about it: the turntable
            // hero (or the honest no-picture card) — see DeviceFigureCard.
            Section {
                DeviceFigureCard(witness: liveWitness)
            }
            .listRowBackground(Color.clear)

            // Who it is, directly under what it looks like — the two halves
            // of one answer, and neither was typed by a human: the figure is
            // resolved from what the device published, the name is derived
            // from its key.
            Section {
                BirthCertificateCard(witness: liveWitness, pairedAt: pairedAt)
            }

            Section("Trust") {
                LabeledContent("Signature") {
                    Label(witness.badge.label, systemImage: witness.badge.sfSymbol)
                        .foregroundStyle(witness.badge.isTrusted ? Theme.color(.calm) : .primary)
                }
                LabeledContent("Chain length", value: "\(witness.chainLength)")
                Button {
                    verifying = true
                    Task { verdict = await verifyNow(); verifying = false }
                } label: {
                    HStack {
                        Text("Verify now")
                        if verifying { Spacer(); ProgressView() }
                        else if let v = verdict { Spacer(); Text(verdictLabel(v)).foregroundStyle(.secondary) }
                    }
                }
            }

            // Where this one stands with a hub, and what to do about it —
            // shown only when there is something to do (HubGuidanceCard draws
            // nothing for a connected hub, and nothing for a device that
            // never said).
            if liveWitness.hub.needsAttention {
                Section { HubGuidanceCard(hub: liveWitness.hub) }
            }

            Section("Health") {
                LabeledContent("Liveness", value: witness.link.label)
                if let r = witness.rssiDBM { LabeledContent("Wi-Fi", value: "\(r) dBm") }
                if let b = witness.batteryPct, b >= 0 { LabeledContent("Battery", value: "\(b)%") }
                if !witness.firmware.isEmpty { LabeledContent("Firmware", value: witness.firmware) }
            }

            // "Where IS it?" — the hot/cold search over this Canary's own
            // Bluetooth beacon, plus (WAP-class) making the device chirp and
            // blink for you. Offered whenever the beacon can be recognized:
            // matching needs the fingerprint, which every paired device and
            // every beacon-attached row carries.
            if !liveWitness.fingerprint.isEmpty {
                Section {
                    NavigationLink {
                        FindCanaryView(witness: liveWitness)
                    } label: {
                        Label("Find this Canary", systemImage: "location.north.circle")
                    }
                } footer: {
                    Text("Warmer/colder over its Bluetooth beacon — room-level, honest words, no fake arrow. A WAP-class Canary can also chirp and blink so it finds you back.")
                }
            }

            if home.isEnabled {
                // The Doctor row: what Apple Home and the fleet each say
                // about this Canary, and one calm line when they disagree.
                Section {
                    LabeledContent("Apple Home", value: standingLabel)
                } footer: {
                    if let note = standing.doctorNote {
                        Label(note, systemImage: "exclamationmark.triangle")
                            .font(.caption)
                            .foregroundStyle(Theme.color(.warn))
                    }
                }
            }

            if witness.deviceType == .wap {
                Section("Two-way") {
                    HStack {
                        Label("AirPlay chime", systemImage: "airplayaudio")
                        Spacer()
                        AirPlayRoutePicker().frame(width: 44, height: 44)
                    }
                }
            }

            if witness.deviceType == .nightlight {
                // The nightlight's own knobs — lamp color/strength, the
                // night schedule, the 12-hour clock — rendered from the
                // device's own /api/settings (the device describes, the
                // app renders).
                NightlightSection(base: nightlightBaseURL)
            }

            // EVERY display serves /api/settings and /api/set, not just the
            // nightlight — the screen brightness, the night window, the red
            // shift and the peek length were all being served to nobody on a
            // Watch Station or a Dash. One screen renders whatever the glass
            // reports, so this needs no per-product branch.
            if witness.deviceType.servesGlassSettings, let base = nightlightBaseURL {
                Section {
                    Button {
                        showingGlassSettings = true
                    } label: {
                        Label("Display settings", systemImage: "slider.horizontal.3")
                    }
                } footer: {
                    Text("Brightness, the night window, how long it lights when you glance at it — and the lamp's color, if it has one. Read from the device, written back to it.")
                }
                .sheet(isPresented: $showingGlassSettings) {
                    GlassSettingsSheet(witness: liveWitness, base: base)
                }
            }

            Section {
                // What this one Canary may interrupt for. Narrowing only —
                // there is deliberately no "never" (WitnessPushFloor): a
                // permanent per-device silence is the failure mode this
                // product can't ship. Unpairing is the honest way to hear
                // nothing, and it looks like what it is.
                Picker("Tell me about this one",
                       selection: Binding(get: { store.pushFloor(for: witness.id) },
                                          set: { store.setPushFloor($0, for: witness.id) })) {
                    ForEach(WitnessPushFloor.allCases) { floor in
                        Text(floor.title).tag(floor)
                    }
                }
            } footer: {
                Text(store.pushFloor(for: witness.id).explanation)
            }

            Section {
                if liveWitness.isMuted {
                    Button("Unmute") { store.unmute(witness.id) }
                        .tint(Theme.color(.warn))
                } else {
                    // Three real choices instead of one fixed hour — a mute
                    // whose length matches the reason for it is a mute people
                    // end up not needing to extend.
                    Menu {
                        ForEach(MuteDuration.offered(at: Date())) { duration in
                            Button {
                                store.mute(witness.id, duration: duration)
                            } label: { Label(duration.title, systemImage: duration.sfSymbol) }
                        }
                    } label: {
                        Text("Mute…")
                    }
                    .tint(Theme.color(.warn))
                }
            } footer: {
                Text(mutedFooter)
            }
        }
        .navigationTitle(witness.displayName)
        .navigationBarTitleDisplayMode(.inline)
    }

    /// When this phone paired it. Nil for a Canary we can see but have not
    /// paired — it has no relationship with us to date, and the certificate
    /// card says "paired" only when there is a pairing to name.
    private var pairedAt: Date? {
        store.devices.devices.first { $0.id == witness.id }?.pairedAt
    }

    /// Says when the quiet ends, because every mute here does end.
    private var mutedFooter: String {
        if let until = liveWitness.mutedUntil, liveWitness.isMuted {
            return "Quiet until \(until.formatted(date: .omitted, time: .shortened)). Tamper and a failed signature still punch through."
        }
        return "A muted Canary stops nagging but stays visible — tamper and a failed signature still punch through. Every mute ends on its own."
    }

    private func verifyNow() async -> ChainVerdict {
        guard let ref = store.devices.devices.first(where: { $0.id == witness.id }),
              let api = try? store.devices.api(for: ref),
              let page = try? await api.witness(last: 100) else {
            return .unsigned
        }
        return ChainVerifier.verify(page, pinnedKey: PinnedKeyStore.key(for: witness.id))
    }

    private func verdictLabel(_ v: ChainVerdict) -> String {
        switch v {
        case .verified: return "Verified ✓"
        case .signedUnpinned: return "Signed"
        case .unsigned: return "Unsigned"
        case .brokenLink(let seq): return "Broken at #\(seq)"
        case .signatureFailed: return "FAILED"
        }
    }
}

#Preview("Device detail — demo Canary") {
    NavigationStack {
        DeviceDetailView(witness: DemoFleet.witnesses().first!)
            .environmentObject(DemoFleet.previewStore())
    }
}
