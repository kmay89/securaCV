// Keychain.swift
//
// Secrets live here, never in UserDefaults, never in the cloud by default:
// per-device tokens and the TOFU-pinned public keys. Items are marked
// `ThisDeviceOnly` so they do NOT ride iCloud Keychain — the default is
// device-bound custody (docs/design/iphone_companion_app.md §10, decision 1).
// A future explicit opt-in can drop that flag; nothing here does it silently.

import Foundation
import Security

struct Keychain {
    enum KError: Error { case status(OSStatus) }

    static func set(_ data: Data, account: String, service: String) throws {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(base as CFDictionary)
        var add = base
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else { throw KError.status(status) }
    }

    static func get(account: String, service: String) -> Data? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var out: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }

    static func delete(account: String, service: String) {
        SecItemDelete([
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ] as CFDictionary)
    }
}

/// Per-device API tokens.
enum TokenStore {
    private static let service = "com.securacv.witness.token"
    static func token(for deviceID: String) -> String? {
        Keychain.get(account: deviceID, service: service).flatMap { String(data: $0, encoding: .utf8) }
    }
    static func set(_ token: String, for deviceID: String) throws {
        try Keychain.set(Data(token.utf8), account: deviceID, service: service)
    }
    static func forget(_ deviceID: String) { Keychain.delete(account: deviceID, service: service) }
}

/// Trust-on-first-use pinned Ed25519 public keys. A key that CHANGES for a
/// device we already pinned is a loud, auditable event (never silent).
enum PinnedKeyStore {
    private static let service = "com.securacv.witness.pinnedkey"

    static func key(for deviceID: String) -> Data? {
        Keychain.get(account: deviceID, service: service)
    }

    /// Returns .pinned on first sight, .matches if identical, .changed if it
    /// differs from what we pinned — the caller must surface `.changed` loudly.
    /// `.notPinned` means the Keychain refused the write: nothing is pinned,
    /// the device stays at the "Signed" rung, and the next sight tries again.
    /// This used to be swallowed (`try?`) and reported as `.pinned`, so a
    /// failed pin looked like a successful one and the trust ladder stalled
    /// one rung down with no signal anywhere.
    @discardableResult
    static func pin(_ key: Data, for deviceID: String) -> PinResult {
        if let existing = Self.key(for: deviceID) {
            return existing == key ? .matches : .changed(previous: existing)
        }
        do {
            try Keychain.set(key, account: deviceID, service: service)
        } catch {
            NSLog("PinnedKeyStore: Keychain refused to pin %@ (%@); staying unpinned until the next sight",
                  deviceID, String(describing: error))
            return .notPinned(reason: String(describing: error))
        }
        return .pinned
    }

    /// Drop the pin — ONLY on unpair (DeviceStore.remove). The pin lives
    /// exactly as long as the pairing it vouches for; while paired, a key is
    /// never forgotten or replaced, which is what makes `.changed` meaningful.
    static func forget(_ deviceID: String) { Keychain.delete(account: deviceID, service: service) }

    enum PinResult: Equatable { case pinned, matches, changed(previous: Data), notPinned(reason: String) }
}
