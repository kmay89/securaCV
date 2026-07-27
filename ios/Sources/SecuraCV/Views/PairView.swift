// PairView.swift
//
// Pairing is a physical-presence gesture, not an account. We reach the Canary,
// ask the user to short-tap its BOOT button (Trust-on-Pair / TOFU), and receive
// the one-shot {device_id, base_url, token} receipt. No cloud registry, no
// login — the device hands you its key because you touched it.

import SwiftUI

struct PairView: View {
    let canary: DiscoveredCanary
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss
    @State private var stage: Stage = .intro
    @State private var receiptText = ""
    @State private var error: String?

    enum Stage { case intro, waiting, manual, done }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: Theme.m) {
                        Image(systemName: canary.deviceType.sfSymbol).font(.title)
                            .foregroundStyle(Theme.color(.info))
                        VStack(alignment: .leading) {
                            Text(canary.name).font(.headline)
                            Text(canary.deviceType.role).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }

                if !canary.deviceType.isHTTPPairable {
                    Section {
                        Label("This Canary is onboarded through Home Assistant over MQTT, not paired here.",
                              systemImage: "info.circle")
                    }
                } else {
                    Section("Prove you're there") {
                        Text("Short-tap the **BOOT** button on \(canary.name). It releases a one-time pairing key — only someone physically at the device can do this.")
                            .font(.subheadline)
                        Button(stage == .waiting ? "Waiting for the tap…" : "I'm ready — start") {
                            stage = .waiting
                        }
                        .disabled(stage == .waiting)
                    }
                    Section("Or paste the receipt") {
                        TextField("{ \"device_id\": …, \"token\": … }", text: $receiptText, axis: .vertical)
                            .lineLimit(2...5).font(.callout.monospaced())
                        Button("Pair from receipt") { pairFromReceipt() }
                            .disabled(receiptText.isEmpty)
                    }
                }

                if let error {
                    Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(Theme.color(.warn)) }
                }
            }
            .navigationTitle("Add Canary")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            }
        }
    }

    private func pairFromReceipt() {
        guard let data = receiptText.data(using: .utf8),
              let receipt = try? JSONDecoder().decode(ProvisioningReceipt.self, from: data),
              DeviceAPI.isPrivate(receipt.baseURL) else {
            error = "That receipt couldn't be read, or points off your local network."
            return
        }
        let ref = PairedDeviceRef(id: receipt.deviceID.isEmpty ? canary.id : receipt.deviceID,
                                  name: canary.name, deviceType: canary.deviceType,
                                  baseURL: receipt.baseURL, pairedAt: Date())
        store.devices.add(ref, token: receipt.token)
        Task { await store.refreshOnce() }
        dismiss()
    }
}
