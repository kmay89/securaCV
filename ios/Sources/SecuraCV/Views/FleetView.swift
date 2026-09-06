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
    @State private var showingFleetWiFi = false
    /// The deep-linked finding target (`securacv://find?witness=…` — the
    /// shell lands the route on this tab; this view consumes the anchor).
    @State private var findTarget: Witness?
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

    /// Canaries on the network that are NOT already in the fleet above.
    ///
    /// "Already in the fleet" is a bigger set than "paired", and treating them
    /// as the same thing is what put a Nightstand in both lists at once — once
    /// under "Your fleet", online and badged, and again under "Discovered on
    /// your network" with a "+" beside it. A display never pairs over HTTP
    /// (it serves no pairing route at all); it JOINS by answering /api/fleet,
    /// and it is a full member of the fleet the moment it does. Offering to
    /// add a device that is already there is an invitation to a flow that
    /// cannot complete, on a device that needs nothing.
    ///
    /// Matched on the id AND on the route, because the two halves of the app
    /// name the same device differently: a self-reported display becomes a
    /// witness keyed `lan:<host>#<index>`, which will never equal the
    /// `device_id` its advert carries. Comparing only ids is precisely why
    /// the filter looked like it was working and wasn't.
    private var unpaired: [DiscoveredCanary] {
        let joined = Set(store.witnesses.map(\.id))
        let joinedHosts = Set(store.witnesses.compactMap { w -> String? in
            guard w.id.hasPrefix("lan:") else { return nil }
            return w.id.dropFirst(4).split(separator: "#").first.map(String.init)
        })
        return store.discovery.found.filter { d in
            if joined.contains(d.deviceID) { return false }
            if let host = d.host, joinedHosts.contains(host) { return false }
            return !store.devices.devices.contains { $0.id == d.deviceID }
        }
    }

    var body: some View {
        NavigationStack {
            Group {
                if showHive && !(store.witnesses.isEmpty && unpaired.isEmpty) {
                    VStack(spacing: 0) {
                        if store.discoveryConsent == nil {
                            DiscoveryConsentCard()
                                .padding([.horizontal, .top])
                        }
                        FleetHiveView(witnesses: filtered, pairing: $pairing)
                    }
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
                        // The one chore that used to be N chores: the router
                        // password changes once, the fleet follows once.
                        // Offered only when there is a real fleet to move —
                        // a demo-only comb has no Wi-Fi to update.
                        if !store.devices.devices.isEmpty || store.witnesses.contains(where: { !$0.id.hasPrefix(DemoFleet.idPrefix) }) {
                            Divider()
                            Button {
                                showingFleetWiFi = true
                            } label: {
                                Label("Update fleet Wi-Fi…", systemImage: "wifi")
                            }
                        }
                    } label: {
                        Label("Options", systemImage: "ellipsis.circle")
                    }
                }
            }
            .navigationDestination(for: Witness.self) { DeviceDetailView(witness: $0) }
            .sheet(item: $pairing) { PairView(canary: $0) }
            .sheet(isPresented: $showingFleetWiFi) { FleetWiFiSheet(store: store) }
            // The find deep link's landing (the complication's door): the
            // search itself, straight away — the row and detail screen are
            // stops the tap already skipped past on purpose. A route naming
            // a witness this fleet doesn't hold consumes to nothing.
            .sheet(item: $findTarget) { target in
                NavigationStack { FindCanaryView(witness: target) }
            }
            .onAppear { consumeFindRoute() }
            .onChange(of: store.pendingRoute) { _, _ in consumeFindRoute() }
            // A cold-launch link can outrun the first refresh; the fold's
            // arrival retries the still-pending route.
            .onChange(of: store.witnesses) { _, _ in consumeFindRoute() }
            .modifier(FleetSearchModifier(query: $query,
                                          enabled: store.witnesses.count >= Self.searchThreshold))
        }
    }

    /// Take a pending find route, if one is ours: resolve the witness and
    /// open the search. On a cold launch the link can outrun the first
    /// refresh, so an EMPTY fleet keeps the route pending (the fold's
    /// arrival retries via onChange) — but once a fold exists, the route is
    /// spent either way: a witness this fleet doesn't hold consumes to
    /// nothing rather than re-firing on every appearance.
    private func consumeFindRoute() {
        guard case .find(let id)? = store.pendingRoute else { return }
        guard !store.witnesses.isEmpty else { return }
        store.pendingRoute = nil
        if let target = store.witnesses.first(where: { $0.id == id }) {
            findTarget = target
        }
    }

    private var fleetList: some View {
        List {
            if store.discoveryConsent == nil {
                Section { DiscoveryConsentCard() }
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets())
            } else if store.discoveryConsent == false {
                Section {
                    DiscoveryOffRow()
                } footer: {
                    Text("Discovery is off — SecuraCV isn't looking for Canaries on this network.")
                }
            }
            if !filtered.isEmpty {
                if store.witnesses.count >= Self.roomGroupThreshold {
                    ForEach(roomGroups, id: \.room) { group in
                        Section(group.room) {
                            ForEach(group.members) { w in
                                NavigationLink(value: w) {
                                    WitnessRow(witness: w,
                                               isNearest: store.nearestWitnessID == w.id)
                                }
                            }
                        }
                    }
                } else {
                    Section(store.demoMode ? "Your fleet (demo)" : "Your fleet") {
                        ForEach(filtered) { w in
                            NavigationLink(value: w) {
                                WitnessRow(witness: w,
                                           isNearest: store.nearestWitnessID == w.id)
                            }
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
                    Text(store.discoveryConsent == true
                         ? "Plug in a Canary on this network — it'll appear here to pair. Or look around with sample data first."
                         : "Enable discovery to find Canaries on this network — or look around with sample data first.")
                } actions: {
                    if store.discoveryConsent != true {
                        Button("Enable discovery") { store.setDiscoveryConsent(true) }
                            .buttonStyle(.borderedProminent)
                        Button("Try the demo fleet") { store.setDemoMode(true) }
                            .buttonStyle(.bordered)
                    } else {
                        Button("Try the demo fleet") { store.setDemoMode(true) }
                            .buttonStyle(.borderedProminent)
                    }
                    // Where a Canary comes from, said here instead of a dead
                    // end: meet one in the Lab (real firmware in the
                    // browser), hatch one with the free Flasher. Routing,
                    // never a funnel — no account behind either link.
                    Link("Where do Canaries come from?",
                         destination: EcosystemMap.labURL)
                        .font(.footnote)
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
    /// The ambient nearness whisper (FleetStore.nearestWitnessID) — a small
    /// glyph beside the name, never an ordering: severity owns the sort.
    var isNearest: Bool = false
    var body: some View {
        HStack(spacing: Theme.m) {
            SeverityPip(severity: witness.effectiveSeverity)
            // What this witness IS, drawn from the same isometric camera every
            // other surface uses (docs/design/FLEET_FIGURES.md). The raw
            // published type resolves first (a "canary-watch" gets the round
            // drum even though the enum collapses it to .unknown); the symbol
            // is the honest fallback — see FleetFigure.resolve.
            DeviceFigureIcon(witness.deviceType, published: witness.publishedType,
                             hardware: witness.hardware, size: 30)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(witness.displayName).font(.body)
                    if witness.isMuted {
                        Image(systemName: "bell.slash").imageScale(.small).foregroundStyle(.secondary)
                    }
                    if witness.seenViaBLE {
                        Image(systemName: "dot.radiowaves.up.forward").imageScale(.small).foregroundStyle(.secondary)
                    }
                    if isNearest {
                        Image(systemName: "location.north.circle")
                            .imageScale(.small)
                            .foregroundStyle(Theme.color(.info))
                            .accessibilityLabel("Nearest to you")
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
            DeviceFigureIcon(canary.deviceType, published: canary.publishedType,
                             hardware: canary.hardware, size: 30)
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
