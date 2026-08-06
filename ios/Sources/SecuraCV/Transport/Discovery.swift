// Discovery.swift
//
// Find Canaries on the LAN with Bonjour/mDNS via Network.framework — the exact
// `_securacv._tcp` service the firmware advertises (canary-vision/docs/
// discovery.md). NWBrowser is a decade-stable OS primitive; no third-party
// dependency, no subnet scanning (honoring the firmware's D5 decision), and it
// keeps working across OS releases. Unknown TXT keys are ignored, not fatal.

import Foundation
import Network

/// A Canary seen on the network before (or without) pairing.
struct DiscoveredCanary: Identifiable, Hashable, Sendable {
    var id: String              // device_id from TXT, or the service name
    var name: String
    var deviceType: DeviceType
    var publishedType: String   // the TXT `dt` verbatim — the enum is coarser
    var host: String?           // mDNS hostname, preferred over IP
    var firmware: String
    var model: String
}

@MainActor
final class Discovery: ObservableObject {
    @Published private(set) var found: [DiscoveredCanary] = []
    @Published private(set) var isBrowsing = false

    private var browser: NWBrowser?

    func start() {
        guard browser == nil else { return }
        let params = NWParameters()
        params.includePeerToPeer = true
        let browser = NWBrowser(for: .bonjourWithTXTRecord(type: "_securacv._tcp", domain: nil), using: params)
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
        var seen: [DiscoveredCanary] = []
        for result in results {
            guard case let .service(name, _, _, _) = result.endpoint else { continue }
            var txt: [String: String] = [:]
            if case let .bonjour(record) = result.metadata {
                txt = record.dictionary
            }
            let dc = DiscoveredCanary(
                id: txt["device_id"] ?? name,
                name: txt["name"] ?? name,
                deviceType: DeviceType(tolerant: txt["dt"]),
                publishedType: txt["dt"] ?? "",
                host: txt["host"],
                firmware: txt["fw"] ?? "",
                model: txt["model"] ?? ""
            )
            seen.append(dc)
        }
        // Stable ordering so the list doesn't jump around as adverts refresh.
        found = seen.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
}
