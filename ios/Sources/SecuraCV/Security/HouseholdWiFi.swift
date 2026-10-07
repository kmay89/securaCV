// HouseholdWiFi.swift
//
// The one secret that makes the second Canary two taps: the household
// Wi-Fi name and password, remembered on this phone — and only this phone —
// after a Canary has actually joined with them. Device-bound Keychain
// (ThisDeviceOnly, never iCloud Keychain, never CloudKit), opt-out on the
// card, clearable from the Set up screen. Never written from a join that
// failed: an unproven password is not a password worth remembering.

import Foundation

struct HouseholdWiFi: Codable, Equatable, Sendable {
    var ssid: String
    var password: String
    var savedAt: Date
}

enum HouseholdWiFiStore {
    private static let service = "com.securacv.witness.household-wifi"
    private static let account = "wifi"

    static func load() -> HouseholdWiFi? {
        guard let data = Keychain.get(account: account, service: service) else { return nil }
        return try? JSONDecoder().decode(HouseholdWiFi.self, from: data)
    }

    static func save(_ wifi: HouseholdWiFi) throws {
        try Keychain.set(try JSONEncoder().encode(wifi), account: account, service: service)
    }

    static func forget() {
        Keychain.delete(account: account, service: service)
    }

    /// Whether a just-finished join should be remembered: only with the
    /// person's toggle on, only when the Canary itself said it joined, and
    /// never an empty name.
    static func shouldRemember(toggleOn: Bool, joined: Bool, ssid: String) -> Bool {
        toggleOn && joined && !ssid.trimmingCharacters(in: .whitespaces).isEmpty
    }
}
