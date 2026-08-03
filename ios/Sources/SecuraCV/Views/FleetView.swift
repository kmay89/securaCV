// FleetView.swift
//
// The fleet, two honest shapes over one truth: the health-ladder LIST (rows
// with liveness, battery, trust) and the HIVE (Honeycomb.swift — the comb
// that fills in as the fleet grows). Automatic picks the hive once there are
// enough Canaries to make a comb worth looking at; either can be pinned from
// the toolbar. At scale the list groups by room and a search field appears —
// ten Canaries should feel as calm as two. "Discovered on your network"
// surfaces Canaries seen over mDNS that aren't paired yet; the hive's "+"
// cell opens the same pairing path.

import SwiftUI

enum FleetViewStyle: String, CaseIterable {
    case auto, hive, list
}

struct FleetView: View {
    @EnvironmentObject var store: FleetStore
    @State private var pairing: DiscoveredCanary?
    @State private var query = ""
    @AppStorage("fleet_view_style") private var styleRaw = FleetViewStyle.auto.rawValue

    /// The comb starts earning its keep around a handful of cells.
    private static let hiveAutoThreshold = 4
    /// Search and room grouping appear only when scale demands them —
    /// chrome nobody needs is chrome somebody has to ignore.
    private static let searchThreshold = 10
    private static let roomGroupThreshold = 6

    private var style: FleetViewStyle { FleetViewStyle(rawValue: styleRaw) ?? .auto }

    private var showHive: Bool {
        switch style {
        case .hive: return true
        case .list: return false
        case .auto: return store.witnesses.count >= Self.hiveAutoThreshold
        }
    }

    private var filtered: [Witness] {
        guard !query.isEmpty else { return store.witnesses }
        return store.witnesses.filter {
            $0.displayName.localizedCaseInsensitiveContains(query)
                || $0.room.localizedCaseInsensitiveContains(query)
        }
    }

    private var unpaired: [DiscoveredCanary] {
        store.discovery.found.filter { d in
            !store.devices.devices.contains { $0.id == d.id }
        }
    }

    var body: some View {
        NavigationStack {
            Group {
                if showHive && !(store.witnesses.isEmpty && unpaired.isEmpty) {
                    FleetHiveView(witnesses: filtered, pairing: $pairing)
                } else {
                    fleetList
                }
            }
            .navigationTitle("Fleet")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Picker("View", selection: $styleRaw) {
                            Label("Automatic", systemImage: "wand.and.stars")
                                .tag(FleetViewStyle.auto.rawValue)
                            Label("Hive", systemImage: "circle.hexagongrid.fill")
                                .tag(FleetViewStyle.hive.rawValue)
                            Label("List", systemImage: "list.bullet")
                                .tag(FleetViewStyle.list.rawValue)
                        }
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
            .modifier(FleetSearchModifier(query: $query,
                                          enabled: store.witnesses.count >= Self.searchThreshold))
        }
    }

    private var fleetList: some View {
        List {
            if !filtered.isEmpty {
                if store.witnesses.count >= Self.roomGroupThreshold {
                    ForEach(roomGroups, id: \.room) { group in
                        Section(group.room) {
                            ForEach(group.members) { w in
                                NavigationLink(value: w) { WitnessRow(witness: w) }
                            }
                        }
                    }
                } else {
                    Section(store.demoMode ? "Your fleet (demo)" : "Your fleet") {
                        ForEach(filtered) { w in
                            NavigationLink(value: w) { WitnessRow(witness: w) }
                        }
                    }
                }
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
                    Label {
                        Text("No Canaries yet")
                    } icon: {
                        CanaryPerchView(height: 64)
                    }
                } description: {
                    Text("Plug in a Canary on this network — it'll appear here to pair. Or look around with sample data first.")
                } actions: {
                    Button("Try the demo fleet") { store.setDemoMode(true) }
                        .buttonStyle(.borderedProminent)
                }
            }
        }
        .refreshable { await store.refreshOnce() }
    }

    /// Rooms alphabetical, the roomless gathered under "Elsewhere" at the
    /// end; inside a room, worst first — same ordering rule as everywhere.
    private var roomGroups: [(room: String, members: [Witness])] {
        let grouped = Dictionary(grouping: filtered) { w in
            w.room.isEmpty ? "Elsewhere" : w.room
        }
        let rooms = grouped.keys.sorted { a, b in
            if a == "Elsewhere" { return false }
            if b == "Elsewhere" { return true }
            return a.localizedCaseInsensitiveCompare(b) == .orderedAscending
        }
        return rooms.map { room in
            (room, grouped[room]!.sorted { $0.effectiveSeverity > $1.effectiveSeverity })
        }
    }
}

/// Search appears only at scale; below the threshold the field would be
/// furniture.
struct FleetSearchModifier: ViewModifier {
    @Binding var query: String
    let enabled: Bool

    func body(content: Content) -> some View {
        if enabled {
            content.searchable(text: $query, prompt: "Name or room")
        } else {
            content
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
