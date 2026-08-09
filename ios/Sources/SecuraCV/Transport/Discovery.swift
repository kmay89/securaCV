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
    /// THE ROW's identity — one per physical device, which is NOT the same
    /// thing as one per published id.
    ///
    /// Prefers the mDNS hostname, because that is the only advertised value
    /// that is already unique per unit: `make_hostname` has always appended
    /// the salted device pseudonym, even on firmware whose `device_id` is the
    /// compile-time model default. Two Dash units both calling themselves
    /// `canary_dash_001` therefore still get two rows here — and they must,
    /// or the second one is never polled and vanishes from the app entirely.
    ///
    /// Falls back to the published id, then the service name, for adverts
    /// with no host at all.
    var id: String
    /// What the device CALLS itself (TXT `device_id`) — the value that
    /// matches a pairing receipt, a paired device, or a witness row. Not
    /// unique across units on firmware older than the personalized seed, so
    /// it must never be used as a row identity. See `id`.
    var deviceID: String
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
        // Unwrap Network.framework's types here and hand the rest to a pure
        // function, so the part with the judgment in it is testable. An
        // NWBrowser.Result cannot be constructed in a test, and the dedupe
        // below is exactly the kind of logic that has already been wrong in
        // both directions — it does not get to live somewhere unreachable.
        var adverts: [(service: String, txt: [String: String])] = []
        for result in results {
            guard case let .service(name, _, _, _) = result.endpoint else { continue }
            if case let .bonjour(record) = result.metadata {
                adverts.append((service: name, txt: record.dictionary))
            } else {
                adverts.append((service: name, txt: [:]))
            }
        }
        found = Self.rows(from: adverts)
    }

    /// Fold the raw adverts into one row per physical device.
    ///
    /// `nonisolated` because it is pure: values in, values out, no access to
    /// anything this actor owns. Without it the method inherits the class's
    /// `@MainActor` isolation and a test — which is not on the main actor —
    /// cannot call it at all, which would have left this logic exactly as
    /// untestable as it was inside the NWBrowser callback. The keyword is the
    /// declaration that this needs no actor, not a workaround for the test.
    nonisolated static func rows(
        from adverts: [(service: String, txt: [String: String])]
    ) -> [DiscoveredCanary] {
        // ONE ROW PER PHYSICAL DEVICE — which is the whole difficulty, because
        // the two obvious keys are each wrong in one direction.
        //
        // Keying on the BROWSE RESULT keeps too many: NWBrowser reports one
        // per (service, interface), and `includePeerToPeer` adds another, so
        // one Canary on Wi-Fi routinely arrives two or three times. That is
        // what listed the same device twice — and it is an `Identifiable`
        // violation too, since those copies share an id, and SwiftUI's ForEach
        // resolves a duplicate id by reusing or dropping rows (so the "+" on
        // one could act on the other).
        //
        // Keying on the PUBLISHED ID keeps too few, and that is the worse
        // failure. `device_id` is a compile-time constant on firmware older
        // than the personalized seed, so every Dash ever flashed answers to
        // `canary_dash_001` — collapsing on it silently drops the second unit,
        // and since FleetStore polls `discovery.found`, that device then
        // disappears from the fleet as well as from this list.
        //
        // The hostname is the key that is right in both directions:
        // `make_hostname` has always appended the salted per-unit pseudonym,
        // so interface copies of ONE device share it and two devices never do.
        var byRow: [String: DiscoveredCanary] = [:]
        for (name, txt) in adverts {
            let host = txt["host"]
            // An empty host is the same as no host: nothing can be dialed at
            // "", so it must not become a row identity either.
            let reachableHost = (host?.isEmpty == false) ? host : nil
            let deviceID = txt["device_id"] ?? name
            let dc = DiscoveredCanary(
                // Host first — see the note on `id`. Only an advert with no
                // host at all falls back to a value two units might share,
                // and such a row cannot be polled anyway.
                id: reachableHost ?? deviceID,
                deviceID: deviceID,
                name: txt["name"] ?? name,
                deviceType: DeviceType(tolerant: txt["dt"]),
                publishedType: txt["dt"] ?? "",
                hardware: txt["hw"] ?? "",
                host: host,
                firmware: txt["fw"] ?? "",
                model: txt["model"] ?? ""
            )
            // Prefer the copy that can actually be reached. The interfaces a
            // service is seen on do not all carry the same TXT payload in
            // practice, and a row with no `host` is a row nothing can poll —
            // so an earlier complete advert must not be replaced by a later
            // bare one just because it arrived second.
            if let existing = byRow[dc.id], existing.host != nil, dc.host == nil { continue }
            byRow[dc.id] = dc
        }
        // Stable ordering so the list doesn't jump around as adverts refresh.
        // Tie-broken by id: two devices CAN still share a name (every unit
        // flashed from one config seeds the same one), and a sort that left
        // them in dictionary order would reshuffle them on every advert.
        return byRow.values.sorted {
            let byName = $0.name.localizedCaseInsensitiveCompare($1.name)
            return byName == .orderedSame ? $0.id < $1.id : byName == .orderedAscending
        }
    }
}
