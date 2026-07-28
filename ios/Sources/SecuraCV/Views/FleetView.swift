// FleetView.swift
//
// The health ladder — one row per Canary, colored by effective severity, with
// liveness, battery, and trust badge. Below, "Discovered on your network"
// surfaces Canaries seen over mDNS that aren't paired yet. Tapping a paired
// device opens its detail (config rendered from the device's own schema).

import SwiftUI

struct FleetView: View {
    @EnvironmentObject var store: FleetStore
    @State private var pairing: DiscoveredCanary?

    var body: some View {
        NavigationStack {
            List {
                if !store.witnesses.isEmpty {
                    Section(store.demoMode ? "Your fleet (demo)" : "Your fleet") {
                        ForEach(store.witnesses) { w in
                            NavigationLink(value: w) { WitnessRow(witness: w) }
                        }
                    }
                }
                let unpaired = store.discovery.found.filter { d in
                    !store.devices.devices.contains { $0.id == d.id }
                }
                if !unpaired.isEmpty {
                    Section("Discovered on your network") {
                        ForEach(unpaired) { d in
                            Button { pairing = d } label: { DiscoveredRow(canary: d) }
                        }
                    }
                }
                if store.witnesses.isEmpty && unpaired.isEmpty {
                    ContentUnavailableView {
                        Label("No Canaries yet", systemImage: "bird")
                    } description: {
                        Text("Plug in a Canary on this network — it'll appear here to pair. Or look around with sample data first.")
                    } actions: {
                        Button("Try the demo fleet") { store.setDemoMode(true) }
                            .buttonStyle(.borderedProminent)
                    }
                }
            }
            .navigationTitle("Fleet")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Toggle("Demo fleet", isOn: Binding(
                            get: { store.demoMode },
                            set: { store.setDemoMode($0) }))
                    } label: {
                        Label("Options", systemImage: "ellipsis.circle")
                    }
                }
            }
            .navigationDestination(for: Witness.self) { DeviceDetailView(witness: $0) }
            .sheet(item: $pairing) { PairView(canary: $0) }
            .refreshable { await store.refreshOnce() }
        }
    }
}

struct WitnessRow: View {
    let witness: Witness
    var body: some View {
        HStack(spacing: Theme.m) {
            SeverityPip(severity: witness.effectiveSeverity)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(witness.displayName).font(.body)
                    if witness.isMuted {
                        Image(systemName: "bell.slash").imageScale(.small).foregroundStyle(.secondary)
                    }
                    if witness.seenViaBLE {
                        Image(systemName: "dot.radiowaves.up.forward").imageScale(.small).foregroundStyle(.secondary)
                    }
                }
                Text(witness.statusLine).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Image(systemName: witness.badge.sfSymbol)
                    .foregroundStyle(witness.badge.isTrusted ? Theme.color(.calm) : .secondary)
                    .imageScale(.small)
                if let b = witness.batteryPct, b >= 0 {
                    Text("\(b)%").font(.caption2).foregroundStyle(.secondary)
                }
            }
        }
        .padding(.vertical, 2)
    }
}

struct DiscoveredRow: View {
    let canary: DiscoveredCanary
    var body: some View {
        HStack(spacing: Theme.m) {
            Image(systemName: canary.deviceType.sfSymbol).foregroundStyle(Theme.color(.info))
            VStack(alignment: .leading, spacing: 2) {
                Text(canary.name).font(.body)
                Text("\(canary.deviceType.role) · tap to pair").font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Image(systemName: "plus.circle").foregroundStyle(Theme.color(.info))
        }
    }
}

#Preview("Fleet — demo fleet") {
    FleetView().environmentObject(DemoFleet.previewStore())
}
