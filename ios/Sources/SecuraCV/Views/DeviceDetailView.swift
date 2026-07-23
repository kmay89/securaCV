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

    var body: some View {
        List {
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

            if witness.deviceType == .wap {
                Section("Two-way") {
                    HStack {
                        Label("AirPlay chime", systemImage: "airplayaudio")
                        Spacer()
                        AirPlayRoutePicker().frame(width: 44, height: 44)
                    }
                }
            }

            Section {
                Button(witness.isMuted ? "Unmute" : "Mute for 1 hour") { }
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
