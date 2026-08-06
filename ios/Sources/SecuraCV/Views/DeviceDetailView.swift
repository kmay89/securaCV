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
        if let host = store.discovery.found.first(where: { $0.id == witness.id })?.host {
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

            Section("Health") {
                LabeledContent("Liveness", value: witness.link.label)
                if let r = witness.rssiDBM { LabeledContent("Wi-Fi", value: "\(r) dBm") }
                if let b = witness.batteryPct, b >= 0 { LabeledContent("Battery", value: "\(b)%") }
                if !witness.firmware.isEmpty { LabeledContent("Firmware", value: witness.firmware) }
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

            Section {
                Button(liveWitness.isMuted ? "Unmute" : "Mute for 1 hour") {
                    if liveWitness.isMuted {
                        store.unmute(witness.id)
                    } else {
                        store.mute(witness.id)
                    }
                }
                .tint(Theme.color(.warn))
            } footer: {
                Text("A muted Canary stops nagging but stays visible — tamper and a failed signature still punch through.")
            }
        }
        .navigationTitle(witness.displayName)
        .navigationBarTitleDisplayMode(.inline)
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
