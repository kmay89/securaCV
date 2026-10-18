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
// did nothing else. The firmware DOES gate a receipt behind a BOOT tap
// (canary-wap and the flagship `firmware/canary` serve
// GET /api/provisioning-receipt for 30 s after the tap, 403 before it) — but
// the receipt a WAP writes points at its own setup-network address, and a
// display serves exactly four API routes, none of them a pairing route. So
// the primary action on the primary onboarding screen was a spinner that
// could never finish for most of the fleet.
//
// It now says what is actually true of the device in front of you, and offers
// an action only where one exists. For a WAP that joined but never paired
// (the one-tap claim was refused, or expired), the real paths are two: its
// recovery kit — the receipt its own setup page saves after a short BOOT
// tap — opened from Files or pasted, with the setup-network address the kit
// names moved onto the address the WAP answers at now
// (ProvisioningReceipt.rebased; the certificate pin is the kit's own); or a
// fresh setup from this phone, which pairs in one tap. Giving a brand-new
// Canary its Wi-Fi in the first place is the Set up walkthrough's job
// (SetupView → CanarySetupView), not this sheet's.

import SwiftUI
import UniformTypeIdentifiers

struct PairView: View {
    let canary: DiscoveredCanary
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss
    @State private var receiptText = ""
    @State private var error: String?
    @State private var importingKit = false

    /// Is this Canary already in the fleet — i.e. did it join by itself while
    /// the user was looking at it? The roster filters joined devices out of
    /// "Discovered", but a device can answer `/api/fleet` for the first time
    /// while this very sheet is open, and the honest thing then is to say so
    /// rather than keep offering to add it.
    private var alreadyJoined: Bool {
        store.witnesses.contains { $0.id == canary.deviceID }
            || store.devices.devices.contains { $0.id == canary.deviceID }
    }

    /// The product name when this build knows it, else the coarse family.
    /// Never the raw wire string: "canary-nightstand7" under a picture of the
    /// device reads like a bug, not like a name.
    private var productLine: String {
        DeviceNaming.productName(published: canary.publishedType,
                                 hardware: canary.hardware)
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
                        Label("Nothing to pair — this kind of Canary doesn't pair with a phone.",
                              systemImage: "info.circle")
                    }
                } else {
                    // WAP-class hardware, the only kind that holds a key worth
                    // handing over. Its key comes one of two real ways.
                    Section {
                        Button {
                            importingKit = true
                        } label: {
                            Label("Open its recovery kit", systemImage: "doc.badge.plus")
                        }
                        TextField("…or paste it: { \"device_id\": …, \"token\": … }", text: $receiptText, axis: .vertical)
                            .lineLimit(2...5).font(.callout.monospaced())
                        Button("Add from what I pasted") { pair(from: Data(receiptText.utf8)) }
                            .disabled(receiptText.isEmpty)
                    } header: {
                        Text("Add it with its recovery kit")
                    } footer: {
                        Text("Its own setup page saves a recovery kit (canary-recovery-kit.json) after a short BOOT tap — that file is its key.")
                    }
                    Section {
                        Text("Or set it up again from this phone: forget its Wi-Fi on its own page (a short BOOT tap lets you in), then bring it near this phone — the card pairs it in one tap.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                        if let page = canary.host.flatMap(DeviceAPI.url(forDiscoveredHost:)) {
                            Link("Open its page", destination: page)
                        }
                    } header: {
                        Text("No recovery kit?")
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
            .fileImporter(isPresented: $importingKit, allowedContentTypes: [.json, .plainText]) { result in
                switch result {
                case .success(let url):
                    // A file from Files is security-scoped: read it inside
                    // the grant, and only it.
                    let scoped = url.startAccessingSecurityScopedResource()
                    defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                    if let data = try? Data(contentsOf: url) {
                        pair(from: data)
                    } else {
                        error = "That file couldn't be read."
                    }
                case .failure:
                    error = "That file couldn't be opened."
                }
            }
        }
    }

    private func pair(from data: Data) {
        guard let decoded = try? JSONDecoder().decode(ProvisioningReceipt.self, from: data) else {
            error = "That isn't a recovery kit or pairing receipt this app can read."
            return
        }
        // A kit saved during setup names the WAP's setup-network address,
        // gone since it joined your Wi-Fi: point it at where THIS device
        // answers now — but only when the kit is this device's (an empty
        // id falls back to the discovered one below, as it always did).
        let sameDevice = decoded.deviceID.isEmpty || decoded.deviceID == canary.deviceID
        let receipt = sameDevice
            ? decoded.rebased(onto: canary.host.flatMap(DeviceAPI.url(forDiscoveredHost:)))
            : decoded
        guard DeviceAPI.isPrivate(receipt.baseURL) else {
            error = "That receipt points off your local network, so it wasn't used."
            return
        }
        // A secure (https) Canary is reachable only through the certificate
        // fingerprint its receipt carries; pairing one without it would add
        // a device every call then refuses (DeviceError.tlsPinMissing). Say
        // so now, at the one moment the user can still get a better receipt.
        if DeviceAPI.isTLS(receipt.baseURL) && receipt.tlsCertFingerprint == nil {
            error = "This receipt points at a secure (https) Canary but carries no certificate "
                + "fingerprint, so the connection couldn't be checked. Update the Canary's "
                + "firmware and take a fresh receipt from its setup page."
            return
        }
        let ref = PairedDeviceRef(id: receipt.deviceID.isEmpty ? canary.deviceID : receipt.deviceID,
                                  name: canary.name, deviceType: canary.deviceType,
                                  baseURL: receipt.baseURL, pairedAt: Date(),
                                  tlsCertFingerprint: receipt.tlsCertFingerprint)
        store.devices.add(ref, token: receipt.token)
        Task { await store.refreshOnce() }
        dismiss()
    }
}
