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
    /// The TXT `hw` — WHICH BOARD this is, as its pins header spells it.
    /// Empty when the device runs firmware older than the field. This is the
    /// only advert key that pins down the SHAPE, so it is what draws the
    /// figure in a discovery row (see FleetFigure.resolve).
    var hardware: String
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
        // ONE ROW PER DEVICE, keyed on identity rather than on the browse
        // result. NWBrowser reports a result per (service, interface), and
        // `includePeerToPeer` adds the peer-to-peer interface on top — so a
        // single Canary sitting on Wi-Fi routinely arrives two or three
        // times. Appending them all is what put the same Canary in the list
        // twice, with the same name and the same id, which reads as two
        // devices you own rather than one device seen twice.
        //
        // It is also an `Identifiable` violation: SwiftUI's ForEach reuses or
        // drops rows when two of them share an id, so the duplicate was not
        // merely ugly — the "+" on one row could act on the other.
        //
        // Keyed on device_id when the advert carries one, falling back to the
        // service name, which is what `id` itself does — so the key is the
        // identity we go on to render, and two rows that would collide in the
        // list collapse here instead.
        var byID: [String: DiscoveredCanary] = [:]
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
                hardware: txt["hw"] ?? "",
                host: txt["host"],
                firmware: txt["fw"] ?? "",
                model: txt["model"] ?? ""
            )
            // Prefer the copy that can actually be reached. The interfaces a
            // service is seen on do not all carry the same TXT payload in
            // practice, and a row with no `host` is a row nothing can poll —
            // so an earlier complete advert must not be replaced by a later
            // bare one just because it arrived second.
            if let existing = byID[dc.id], existing.host != nil, dc.host == nil { continue }
            byID[dc.id] = dc
        }
        // Stable ordering so the list doesn't jump around as adverts refresh.
        // Tie-broken by id: two devices CAN still share a name (every unit
        // flashed from one config seeds the same one), and a sort that left
        // them in dictionary order would reshuffle them on every advert.
        found = byID.values.sorted {
            let byName = $0.name.localizedCaseInsensitiveCompare($1.name)
            return byName == .orderedSame ? $0.id < $1.id : byName == .orderedAscending
        }
    }
}
