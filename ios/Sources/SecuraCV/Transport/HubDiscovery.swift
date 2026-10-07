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

    private var browser: NWBrowser?

    func start() {
        guard browser == nil else { return }
        let params = NWParameters()
        params.includePeerToPeer = false
        let browser = NWBrowser(for: .bonjourWithTXTRecord(type: HubDiscoveryRows.serviceType, domain: nil),
                                using: params)
        self.browser = browser
        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                switch state {
                case .ready: self?.isBrowsing = true
                case .failed, .cancelled: self?.isBrowsing = false
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
