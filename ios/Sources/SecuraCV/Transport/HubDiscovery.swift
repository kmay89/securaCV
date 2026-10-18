// HubDiscovery.swift
//
// Find Home Assistant on the LAN the way the phone finds Canaries: Bonjour
// over Network.framework, no subnet scanning, no third-party dependency.
// Home Assistant advertises `_home-assistant._tcp` with its address in the
// TXT record (base_url / internal_url, plus version, location_name and the
// instance uuid), so a hub that just finished first boot appears here by
// itself — the owner never types `homeassistant.local`, though they can.
// The fold into rows is pure (HubDiscoveryRows) and host-tested.

import Foundation
import Network

@MainActor
final class HubDiscovery: ObservableObject {
    @Published private(set) var found: [DiscoveredHub] = []
    @Published private(set) var isBrowsing = false
    /// iOS refused the browse because Local Network access is off for this
    /// app. NWBrowser says so by WAITING (not failing) with the DNS-SD
    /// policy-denied error, and it moves on to .ready by itself once the
    /// person turns the switch on in Settings — so this clears on its own
    /// too. Read by the setup screens, which say why instead of spinning.
    @Published private(set) var localNetworkBlocked = false

    private var browser: NWBrowser?

    func start() {
        guard browser == nil else { return }
        let params = NWParameters()
        params.includePeerToPeer = false
        let browser = NWBrowser(for: .bonjourWithTXTRecord(type: HubDiscoveryRows.serviceType, domain: nil),
                                using: params)
        self.browser = browser
        browser.stateUpdateHandler = { [weak self, weak browser] state in
            Task { @MainActor in
                switch state {
                case .ready:
                    self?.isBrowsing = true
                    self?.localNetworkBlocked = false
                case .waiting(let error):
                    self?.isBrowsing = false
                    self?.localNetworkBlocked = Discovery.isPolicyDenied(error)
                case .failed:
                    // A failed browser never recovers; drop it so the next
                    // start() (the next foreground) builds a fresh one.
                    // (Only if it is still the current one — a stop/start
                    // may already have replaced it.)
                    self?.isBrowsing = false
                    if let self, self.browser === browser { self.browser = nil }
                case .cancelled:
                    self?.isBrowsing = false
                default: break
                }
            }
        }
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            Task { @MainActor in self?.ingest(results) }
        }
        browser.start(queue: .main)
    }

    func stop() {
        browser?.cancel()
        browser = nil
        isBrowsing = false
    }

    private func ingest(_ results: Set<NWBrowser.Result>) {
        var adverts: [(service: String, txt: [String: String])] = []
        for result in results {
            guard case let .service(name, _, _, _) = result.endpoint else { continue }
            if case let .bonjour(record) = result.metadata {
                adverts.append((service: name, txt: record.dictionary))
            } else {
                adverts.append((service: name, txt: [:]))
            }
        }
        found = HubDiscoveryRows.rows(from: adverts)
    }
}
