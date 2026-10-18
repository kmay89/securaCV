// HouseholdWiFi.swift
//
// The one secret that makes the second Canary two taps: the household
// Wi-Fi name and password, remembered on this phone — and only this phone —
// after a Canary has actually joined with them. Every path that asks for
// Wi-Fi opens with it (the nearby card, a setup network), every path that
// proves a join may save it, and "Update fleet Wi-Fi" keeps it current. Device-bound Keychain
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

    /// What a Wi-Fi form opens with, on every path that asks (the nearby
    /// card, a setup network, the fleet Wi-Fi update): the remembered
    /// network — but never over something the person already typed.
    static func prefill(typedSSID: String, remembered: HouseholdWiFi?) -> (ssid: String, password: String)? {
        guard typedSSID.trimmingCharacters(in: .whitespaces).isEmpty, let remembered,
              !remembered.ssid.trimmingCharacters(in: .whitespaces).isEmpty else { return nil }
        return (remembered.ssid, remembered.password)
    }

    /// After "Update fleet Wi-Fi": replace the remembered network only when
    /// the person had already asked this phone to remember one (the
    /// rollout is not a fresh opt-in), and only once a Canary proved the
    /// new password by coming back on it. Otherwise the next Canary would
    /// be prefilled with the old password and fail with "check the
    /// password" — for a password the person had just changed.
    static func shouldReplace(existing: HouseholdWiFi?, anyMoved: Bool, ssid: String) -> Bool {
        existing != nil && shouldRemember(toggleOn: true, joined: anyMoved, ssid: ssid)
    }
}
