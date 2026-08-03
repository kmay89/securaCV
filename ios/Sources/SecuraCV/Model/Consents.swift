// Consents.swift
//
// The user says yes BEFORE any radio moves — and therefore before iOS shows
// its Local Network / Bluetooth permission dialogs, so those arrive in
// context instead of ambushing the first launch (the Apple-recommended
// pre-permission pattern, and plain good manners). Tri-state on purpose:
// "never asked" is a real state, distinct from an explicit no — the UI
// invites on the first, stays quiet-but-findable on the second, and nags on
// neither. Pure over injected UserDefaults; host-tested.

import Foundation

enum Consents {
    static let discoveryKey = "consent_discovery_v1"

    /// nil = never asked · true = granted · false = declined ("Not now").
    static func discovery(in defaults: UserDefaults = .standard) -> Bool? {
        defaults.object(forKey: discoveryKey) as? Bool
    }

    static func setDiscovery(_ granted: Bool, in defaults: UserDefaults = .standard) {
        defaults.set(granted, forKey: discoveryKey)
    }
}
