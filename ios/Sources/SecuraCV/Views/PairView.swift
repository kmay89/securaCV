// PairView.swift
//
// What it takes to add THIS Canary — which, for most of the fleet, is nothing.
//
// Pairing is a physical-presence gesture, not an account: a device hands you
// its key because you touched it, with no cloud registry and no login. But
// only WAP-class hardware works that way. Everything else joins by answering
// `/api/fleet` on the LAN, and is already a full member of the fleet by the
// time this sheet could open.
//
// THE SCREEN THIS REPLACED offered every discovered Canary a "Short-tap the
// BOOT button — I'm ready, start" button. That button set a state variable and
// did nothing else, because there is nothing for it to do: no firmware in this
// repo serves a BOOT-tap receipt endpoint, on any board. A display serves
// exactly four API routes and none of them is a pairing route. So the primary
// action on the primary onboarding screen was a spinner that could never
// finish, describing a mechanism that does not exist.
//
// It now says what is actually true of the device in front of you, and offers
// an action only where one exists. The receipt paste stays: it is the one path
// that has always worked, for anything that can produce a receipt.

import SwiftUI

struct PairView: View {
    let canary: DiscoveredCanary
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss
    @State private var receiptText = ""
    @State private var error: String?

    /// Is this Canary already in the fleet — i.e. did it join by itself while
    /// the user was looking at it? The roster filters joined devices out of
    /// "Discovered", but a device can answer `/api/fleet` for the first time
    /// while this very sheet is open, and the honest thing then is to say so
    /// rather than keep offering to add it.
    private var alreadyJoined: Bool {
        store.witnesses.contains { $0.id == canary.id }
            || store.devices.devices.contains { $0.id == canary.id }
    }

    /// The product name when this build knows it, else the coarse family.
    /// Never the raw wire string: "canary-nightstand7" under a picture of the
    /// device reads like a bug, not like a name.
    private var productLine: String {
        DeviceNaming.productTitle(forPublishedType: canary.publishedType)
            ?? canary.deviceType.role
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: Theme.m) {
                        // The device itself, drawn from what it published —
                        // the same figure the roster shows, so the thing you
                        // are about to add looks like the thing in the room.
                        DeviceFigureIcon(canary.deviceType, published: canary.publishedType,
                                         hardware: canary.hardware, size: 40)
                        VStack(alignment: .leading) {
                            Text(canary.name).font(.headline)
                            Text(productLine).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }

                if alreadyJoined {
                    Section {
                        Label("Already in your fleet — it joined by itself.",
                              systemImage: "checkmark.circle")
                            .foregroundStyle(Theme.color(.calm))
                    } footer: {
                        Text("You can close this. It's on the Fleet tab, and its settings are there too.")
                    }
                } else if canary.deviceType.servesGlassSettings {
                    // A display. It joins the fleet by answering /api/fleet, so
                    // there is nothing to do here and nothing to tap — saying
                    // so is the whole content of this screen.
                    Section {
                        Label("Nothing to do — displays join on their own.",
                              systemImage: "sparkles")
                    } footer: {
                        Text("\(canary.name) is on your network and answering. It'll appear on the Fleet tab within a few seconds of being seen, and you can change its screen settings from there. Displays don't hold keys of their own, so there's no key to hand over.")
                    }
                } else if !canary.deviceType.isHTTPPairable {
                    Section {
                        Label("Onboarded through Home Assistant over MQTT, not paired here.",
                              systemImage: "info.circle")
                    }
                } else {
                    // WAP-class hardware, the only kind that holds a key worth
                    // handing over. Its onboarding lives on the device's own
                    // setup portal, which issues the receipt below — this app
                    // has no way to ask for one over the LAN.
                    Section("Add this Canary") {
                        Text("\(canary.name) hands over its key from its own setup page, which you reach by joining its Wi-Fi while it's in setup mode. That page gives you a receipt — paste it below.")
                            .font(.subheadline)
                    }
                    Section("Paste the receipt") {
                        TextField("{ \"device_id\": …, \"token\": … }", text: $receiptText, axis: .vertical)
                            .lineLimit(2...5).font(.callout.monospaced())
                        Button("Add from receipt") { pairFromReceipt() }
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
