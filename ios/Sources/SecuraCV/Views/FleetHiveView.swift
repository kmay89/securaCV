// FleetHiveView.swift
//
// The fleet as a comb: one cell per Canary, severity as a colored ring, the
// device's kind as the glyph — and always one dashed "+" cell, so the next
// Canary's place in the hive is visibly waiting for it. Calm by design: a
// quiet fleet is a wall of soft green rings, and the one cell that needs
// you is the only one saturated. Tapping a cell opens the same detail the
// list opens; the comb is a view, never a second implementation.

import SwiftUI

struct FleetHiveView: View {
    @EnvironmentObject var store: FleetStore
    let witnesses: [Witness]
    @Binding var pairing: DiscoveredCanary?
    @State private var showAdd = false

    var body: some View {
        ScrollView {
            VStack(spacing: Theme.l) {
                if store.demoMode { DemoDataBanner() }
                HoneycombLayout(diameter: 96, gap: 10) {
                    ForEach(witnesses) { w in
                        NavigationLink(value: w) { HiveCell(witness: w) }
                            .buttonStyle(.plain)
                            .accessibilityLabel("\(w.displayName), \(w.effectiveSeverity.label)")
                    }
                    Button {
                        showAdd = true
                    } label: {
                        AddHiveCell()
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Add a Canary")
                }
                Text(growthLine)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
            }
            .padding()
        }
        .refreshable { await store.refreshOnce() }
        .sheet(isPresented: $showAdd) {
            AddCanarySheet(pairing: $pairing)
        }
    }

    /// Warmth, not pressure: name what's here, hint at the empty cell.
    private var growthLine: String {
        switch witnesses.count {
        case 0: return "Your hive is ready — plug in a Canary and it appears here."
        case 1: return "One Canary on watch. There's a cell waiting for its first fleetmate."
        default: return "\(witnesses.count) Canaries watching together."
        }
    }
}

struct HiveCell: View {
    let witness: Witness

    private var role: Theme.Role { witness.effectiveSeverity.role }
    private var isQuiet: Bool { witness.effectiveSeverity == .ok }

    var body: some View {
        ZStack {
            Circle().fill(.ultraThinMaterial)
            // Quiet cells get a soft ring; the one that needs you is the
            // only one at full saturation. Color never carries meaning
            // alone — the glyph and label repeat it.
            Circle()
                .strokeBorder(Theme.color(role).opacity(isQuiet ? 0.35 : 0.95),
                              lineWidth: isQuiet ? 2.5 : 3.5)
            VStack(spacing: 3) {
                // The device itself where the pipeline can draw it honestly —
                // a 96 pt cell is the roster's best canvas for "which thing is
                // this?" — with the severity-tinted symbol as the fallback.
                // Severity still reads from the ring and never from the figure:
                // the figure's colors are the object's, not the theme's.
                if let figure = FleetFigure.resolve(deviceType: witness.deviceType,
                                                    published: witness.publishedType) {
                    FleetFigureView(figure)
                        .frame(width: 34, height: 34)
                        .accessibilityLabel(figure.title)
                } else {
                    Image(systemName: witness.deviceType.sfSymbol)
                        .font(.title3)
                        .foregroundStyle(isQuiet ? .secondary : Theme.color(role))
                }
                Text(witness.displayName)
                    .font(.caption2)
                    .lineLimit(1)
                    .frame(maxWidth: 76)
                if witness.isMuted {
                    Image(systemName: "bell.slash.fill")
                        .font(.system(size: 8))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(6)
        }
    }
}

struct AddHiveCell: View {
    var body: some View {
        ZStack {
            Circle()
                .strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [6, 5]))
                .foregroundStyle(.secondary.opacity(0.6))
            VStack(spacing: 3) {
                Image(systemName: "plus")
                    .font(.title3)
                Text("Add")
                    .font(.caption2)
            }
            .foregroundStyle(.secondary)
        }
    }
}

/// The "+" cell's sheet: Canaries seen on the network, ready to pair — or
/// the honest words when there are none yet. Pairing itself is the same
/// PairView as everywhere.
struct AddCanarySheet: View {
    @EnvironmentObject var store: FleetStore
    @Binding var pairing: DiscoveredCanary?
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                let unpaired = store.discovery.found.filter { d in
                    !store.devices.devices.contains { $0.id == d.id }
                }
                if store.discoveryConsent != true {
                    Section {
                        VStack(alignment: .leading, spacing: Theme.s) {
                            Label("Discovery is off", systemImage: "antenna.radiowaves.left.and.right.slash")
                                .font(.headline)
                            Text("SecuraCV only looks for Canaries on your network after you say so. Enable discovery and any Canary on this Wi-Fi appears here by itself.")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                            Button("Enable discovery") { store.setDiscoveryConsent(true) }
                                .buttonStyle(.borderedProminent)
                        }
                        .padding(.vertical, Theme.xs)
                    }
                } else if unpaired.isEmpty {
                    Section {
                        VStack(alignment: .leading, spacing: Theme.s) {
                            Label("No new Canaries in sight", systemImage: "wifi.slash")
                                .font(.headline)
                            Text("Plug a Canary into power on this Wi-Fi and it appears here by itself — no accounts, no setup codes.")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                        .padding(.vertical, Theme.xs)
                    }
                } else {
                    Section("Found on your network") {
                        ForEach(unpaired) { d in
                            Button {
                                dismiss()
                                // Hand off to FleetView's pairing sheet after
                                // this one has left the stage.
                                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                                    pairing = d
                                }
                            } label: {
                                DiscoveredRow(canary: d)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Add a Canary")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}

#Preview("Hive — demo fleet") {
    NavigationStack {
        FleetHiveView(witnesses: DemoFleet.witnesses(), pairing: .constant(nil))
            .environmentObject(DemoFleet.previewStore())
    }
}
