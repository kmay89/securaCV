// HubDiscoveryRows.swift
//
// Home Assistant advertises itself on the LAN as `_home-assistant._tcp`,
// with its address in the TXT record — the phone needs no typed hostname
// to find a hub that just booted. This is the pure half of that browse:
// adverts in, one row per instance out, every address held to the same
// "only on this network" gate as a Canary's.

import Foundation

/// A Home Assistant instance seen on the network.
struct DiscoveredHub: Identifiable, Hashable, Sendable {
    /// The instance's own uuid when it published one, else the service name.
    var id: String
    /// The location name the owner gave it ("Home"), or the service name.
    var name: String
    /// Where to dial it: the advertised internal/base URL, if private.
    var baseURL: URL?
    var version: String
    /// "Home Assistant OS", "Home Assistant Container", … — as published.
    var installationType: String
}

enum HubDiscoveryRows {
    static let serviceType = "_home-assistant._tcp"

    /// Fold the raw adverts into rows. The address comes from `internal_url`
    /// first (what the frontend uses on the LAN), then `base_url`; one that
    /// fails the private-host gate is dropped rather than dialed, and a hub
    /// with no usable address still gets a row so the person can type one.
    static func rows(from adverts: [(service: String, txt: [String: String])]) -> [DiscoveredHub] {
        var byID: [String: DiscoveredHub] = [:]
        for (service, txt) in adverts {
            let id = (txt["uuid"]?.isEmpty == false ? txt["uuid"] : nil) ?? service
            let candidates = [txt["internal_url"], txt["base_url"]].compactMap { $0 }
            let url = candidates.lazy
                .compactMap { URL(string: $0) }
                .first { DeviceAPI.isPrivate($0) }
            let row = DiscoveredHub(
                id: id,
                name: txt["location_name"].flatMap { $0.isEmpty ? nil : $0 } ?? service,
                baseURL: url,
                version: txt["version"] ?? "",
                installationType: txt["installation_type"] ?? "")
            // Interface copies of one instance collapse into one row: the
            // first address that passed the gate wins, and a field one copy
            // left blank is filled from the other, whichever order the
            // copies arrive in. Overwriting would lose a field the first
            // copy carried and the second did not.
            byID[id] = byID[id].map { $0.merged(with: row, service: service) } ?? row
        }
        return byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
}

private extension DiscoveredHub {
    /// This row with every blank filled from `other`. The address already
    /// held is kept; a name that is only the service name yields to one the
    /// owner gave the instance.
    func merged(with other: DiscoveredHub, service: String) -> DiscoveredHub {
        DiscoveredHub(
            id: id,
            name: (name == service && other.name != service) ? other.name : name,
            baseURL: baseURL ?? other.baseURL,
            version: version.isEmpty ? other.version : version,
            installationType: installationType.isEmpty ? other.installationType : installationType)
    }
}
