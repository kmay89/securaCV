// DiscoveryConsentCard.swift
//
// The ask-before-the-radios card. Nothing browses, nothing scans — and iOS
// shows no Local Network / Bluetooth dialog — until the user taps yes here,
// so the system prompt lands right after their own decision instead of
// ambushing the first launch. ONE prompt, too: the tap starts the Bluetooth
// scan only (what finds a brand-new Canary); the mDNS browse and its Local
// Network prompt wait for the first thing that needs the Wi-Fi searched
// (FleetStore.startLocalNetworkDiscoveryIfConsented). "Not now" is a real
// answer: the card goes away and a quiet, findable row remains (Fleet list
// + the Add sheet) — an invitation that waits, never a nag.

import SwiftUI

struct DiscoveryConsentCard: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: Theme.s) {
                HStack(spacing: Theme.s) {
                    CanaryPerchView(height: 40)
                    Label("Find Canaries on this Wi-Fi", systemImage: "antenna.radiowaves.left.and.right")
                        .font(.headline)
                        .labelStyle(.titleOnly)
                }
                Text("SecuraCV looks for Canaries on your own network and listens for their Bluetooth beacons — that is also how it hears a brand-new one. Everything stays between your devices; nothing touches any company server. iOS asks for Bluetooth next — that prompt is this feature — and for Local Network only once there is a Canary to find on your Wi-Fi.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                HStack(spacing: Theme.s) {
                    Button("Enable discovery") { store.setDiscoveryConsent(true) }
                        .buttonStyle(.borderedProminent)
                    Button("Not now") { store.setDiscoveryConsent(false) }
                        .buttonStyle(.bordered)
                }
            }
        }
    }
}

/// The quiet comeback for an earlier "Not now" — one row, no lecture.
struct DiscoveryOffRow: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        Button {
            store.setDiscoveryConsent(true)
        } label: {
            Label("Turn on discovery", systemImage: "antenna.radiowaves.left.and.right")
        }
    }
}

#Preview("Consent card") {
    DiscoveryConsentCard()
        .environmentObject(DemoFleet.previewStore())
        .padding()
}
