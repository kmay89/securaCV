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
            // Interface copies of one instance collapse. The first address
            // that passed the gate wins, and a field one copy left out is
            // taken from a copy that carried it, so the row does not depend
            // on which copy the browse happened to see last.
            guard var kept = byID[id] else {
                byID[id] = row
                continue
            }
            if kept.baseURL == nil { kept.baseURL = row.baseURL }
            if kept.version.isEmpty { kept.version = row.version }
            if kept.installationType.isEmpty { kept.installationType = row.installationType }
            byID[id] = kept
        }
        return byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
}
