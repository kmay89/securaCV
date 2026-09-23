// Keychain.swift
//
// Secrets live here, never in UserDefaults, never in the cloud by default:
// per-device tokens, the TOFU-pinned public keys, and the sealed-snapshot
// operator key. Items are marked `ThisDeviceOnly` so they do NOT ride
// iCloud Keychain — the default is device-bound custody
// (docs/design/iphone_companion_app.md §10, decision 1).
// A future explicit opt-in can drop that flag; nothing here does it silently.

import CryptoKit
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

/// A few named secrets under one Keychain service — the seam the stores
/// below take in `init` (the app's `AlertCenter(defaults:)` idiom), so a
/// test hands them a dictionary instead. Not a nicety: CI builds the app
/// unsigned (scripts/heal.sh, CODE_SIGNING_ALLOWED=NO), and whether an
/// unsigned simulator app may use the Keychain at all is the host's call,
/// not the code's — a test that leaned on it would test the runner.
protocol SecretSlots {
    func get(_ account: String) -> Data?
    func set(_ data: Data, _ account: String) throws
    func delete(_ account: String)
}

/// The production slots: generic-password items, `ThisDeviceOnly`.
struct KeychainSlots: SecretSlots {
    let service: String
    func get(_ account: String) -> Data? { Keychain.get(account: account, service: service) }
    func set(_ data: Data, _ account: String) throws {
        try Keychain.set(data, account: account, service: service)
    }
    func delete(_ account: String) { Keychain.delete(account: account, service: service) }
}

/// The sealed-snapshot operator key: the X25519 private half that every
/// `.svlt` a Canary seals for this phone is encrypted to. Generated HERE,
/// once, and never exported — the Canary is handed only the public half
/// (`DeviceAPI.vaultRegisterKey`), which is the whole promise: a device that
/// holds only a public key is structurally unable to open what it sealed.
///
/// Two Keychain items, on purpose: the private key, and the public key
/// beside it. Every screen that names the key (its id, its hex, "registered
/// on this Canary?") reads the PUBLIC item, so the private one is touched
/// only by an actual unseal — the shape a presence-gated custody needs.
///
/// Not the kernel's break-glass vault. That opens by quorum (Invariant V)
/// and this app has no path into it; this is one recipient's key for
/// single-recipient frames (docs/sealed_snapshot_vault.md). The Keychain
/// service keeps the format's own word ("vault", as in /api/vault/* and
/// the `/VAULT` ring) because it is an identifier; every string a person
/// reads says "snapshot key".
struct VaultKeyStore {
    static let service = "com.securacv.vault.key"
    static let account = "operator-x25519-v1"
    static let publicAccount = "operator-x25519-v1.pub"

    /// The one the app uses.
    static let app = VaultKeyStore(slots: KeychainSlots(service: service))

    let slots: SecretSlots

    /// The copy every "Forget" confirmation shows. Loud because it is true:
    /// there is no recovery, no sync, no second copy anywhere.
    static let forgetWarning =
        "Every snapshot sealed to this key becomes unreadable — on this phone, on every "
        + "Canary that sealed one, forever. There is no backup and no way to recover it. "
        + "Canaries you registered it on keep sealing to it until you register a new key."

    /// True when a key exists on this phone. Reads the public item only.
    var exists: Bool { publicKeyRaw != nil }

    /// The raw 32-byte public key, or nil when no key has been created.
    var publicKeyRaw: Data? {
        if let pub = slots.get(Self.publicAccount), pub.count == 32 {
            return pub
        }
        // A private item with no public twin (a write that half-failed):
        // derive and repair, once.
        guard let key = try? privateKey() else { return nil }
        let pub = key.publicKey.rawRepresentation
        try? slots.set(pub, Self.publicAccount)
        return pub
    }

    /// 64 lowercase hex — what `POST /api/vault/key` wants as `pubkey`.
    var publicKeyHex: String? { publicKeyRaw.map(ChainVerifier.hex) }

    /// 16 hex — SHA-256(pub)[0..<8], the id a sealed file's header carries
    /// and the one `/api/vault/status` reports as `key_id`.
    var keyIDHex: String? {
        publicKeyRaw.map { ChainVerifier.hex(SnapshotSealer.keyID(ofRawPublicKey: $0)) }
    }

    /// The private key, for an unseal. Nil when none exists; throws when
    /// the stored bytes are not a key (never a guess at one).
    func privateKey() throws -> Curve25519.KeyAgreement.PrivateKey? {
        guard let raw = slots.get(Self.account) else { return nil }
        return try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: raw)
    }

    /// Create the key if this phone has none; return the one it has.
    @discardableResult
    func generateIfNeeded() throws -> Curve25519.KeyAgreement.PrivateKey {
        if let existing = try privateKey() { return existing }
        let key = Curve25519.KeyAgreement.PrivateKey()
        try slots.set(key.rawRepresentation, Self.account)
        try slots.set(key.publicKey.rawRepresentation, Self.publicAccount)
        return key
    }

    /// Delete the key. The caller has shown `forgetWarning` and had it
    /// confirmed — this method does not ask, it does.
    func forget() {
        slots.delete(Self.account)
        slots.delete(Self.publicAccount)
    }
}
