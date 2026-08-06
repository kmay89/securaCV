// NetworkVantage.swift — the two permission-free facts HomePresence needs.
//
// "Is this phone on Wi-Fi?" and "has the fleet ever answered on the network
// it is currently attached to?" Together they turn "no Canary is answering"
// from a guess into a decision — see HomePresence for why that mattered
// enough to build.
//
// Deliberately NOT here: SSID, BSSID, location, geofences. Reading the SSID
// needs an entitlement and (since iOS 13) location permission, and asking for
// someone's location to decide whether to send a smoke alert is a trade this
// project does not make. NWPathMonitor needs nothing, is reported by the OS,
// and answers the only question we actually have.
//
// The network-generation counter is the whole trick. NWPathMonitor hands us a
// fresh path whenever the association changes — join a different Wi-Fi, drop
// to cellular, come back — so "did the fleet answer during THIS association?"
// is a counter comparison, with no notion of network identity to store, leak,
// or get wrong.

import Foundation
import Network

@MainActor
final class NetworkVantage: ObservableObject {
    static let shared = NetworkVantage()

    /// On a Wi-Fi (or wired) link, as opposed to cellular or nothing at all.
    /// Cellular is the honest "certainly not on the home LAN" signal.
    @Published private(set) var onWiFi = false

    /// Has at least one Canary answered since the current network attachment
    /// began? False on a network we have never heard the fleet on — a guest
    /// network, a coffee shop — which is what separates "away" from "the
    /// house went dark".
    @Published private(set) var sawFleetOnThisNetwork = false

    /// Bumped by the OS on every path change; the only thing standing in for
    /// network identity, and it never leaves the device.
    private var generation = 0
    private var monitor: NWPathMonitor?

    private init() {}

    func start() {
        guard monitor == nil else { return }
        let monitor = NWPathMonitor()
        self.monitor = monitor
        monitor.pathUpdateHandler = { [weak self] path in
            let wifi = path.status == .satisfied
                && !path.usesInterfaceType(.cellular)
            Task { @MainActor in self?.note(onWiFi: wifi) }
        }
        monitor.start(queue: .main)
    }

    func stop() {
        monitor?.cancel()
        monitor = nil
    }

    /// Called on every path update. A changed vantage resets the "seen here"
    /// claim, because it is a claim about the network we just left.
    private func note(onWiFi wifi: Bool) {
        guard wifi != onWiFi else { return }
        onWiFi = wifi
        generation += 1
        sawFleetOnThisNetwork = false
    }

    /// FleetStore calls this whenever a Canary is actually answering. Latches
    /// for the life of the current attachment: the fleet does not stop living
    /// on this network the moment it goes quiet — that quiet is the news.
    func noteFleetSeen() {
        guard !sawFleetOnThisNetwork else { return }
        sawFleetOnThisNetwork = true
    }

    /// Test seam: drive the two facts directly, without a real path monitor.
    /// `HomePresence` holds the policy and is tested on its own; this only
    /// needs its latch-and-reset behavior pinned.
    func _setForTesting(onWiFi wifi: Bool, sawFleet: Bool) {
        onWiFi = wifi
        sawFleetOnThisNetwork = sawFleet
    }

    /// Test seam: simulate the OS reporting a different network attachment.
    func _simulatePathChangeForTesting(onWiFi wifi: Bool) {
        onWiFi = !wifi          // force the guard in `note` to fire
        note(onWiFi: wifi)
    }
}
