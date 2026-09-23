// Keychain.swift
//
// Secrets live here, never in UserDefaults, never in the cloud by default:
// per-device tokens, the TOFU-pinned public keys, and the sealed-snapshot
// operator key. Items are marked `ThisDeviceOnly` so they do NOT ride
// iCloud Keychain — the default is device-bound custody
// (docs/design/iphone_companion_app.md §10, decision 1).
// A future explicit opt-in can drop that flag; nothing here does it silently.
//
// The sealed-snapshot key is also WRAPPED (EnclaveCustody.swift): through
// the Secure Enclave, behind Face ID / Touch ID / the passcode, where the
// device has one. Tokens and pinned keys are not, by design: a token is read
// on every 10–20 s poll, and a presence prompt per poll is no product; a
// pinned key is public (integrity, not secrecy).

import CryptoKit
import Foundation
import LocalAuthentication
import Security

struct Keychain {
    enum KError: Error { case status(OSStatus) }

    /// Store `data`, replacing any value already there — atomically. An
    /// existing item is updated in place (SecItemUpdate either replaces the
    /// value or leaves the old one), and a new one is added only when there
    /// was none. Never delete-then-add: if the add then failed, the previous
    /// value would be gone, and for the snapshot key that is every snapshot
    /// ever sealed to it.
    static func set(_ data: Data, account: String, service: String) throws {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let fields: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        ]
        let updated = SecItemUpdate(base as CFDictionary, fields as CFDictionary)
        if updated == errSecSuccess { return }
        guard updated == errSecItemNotFound else { throw KError.status(updated) }
        let add = base.merging(fields) { _, new in new }
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
///
/// Contract: `set` replaces atomically — when it throws, the previous value
/// is still there. The snapshot key's in-place migration (raw → wrapped)
/// relies on it: a failed wrap must leave the raw key, never nothing.
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
/// on this Canary?", its custody) reads the PUBLIC item or the private
/// item's first byte, so the private key is unwrapped only by an actual
/// unseal — which is what lets that unwrap ask for Face ID.
///
/// The private item is a custody record, `kind tag ‖ envelope`
/// (EnclaveCustody.swift): the record names the wrapper that can open it.
/// A 32-byte item is the pre-custody layout (the raw key) and is wrapped in
/// place the first time the store sees it — `migrateIfNeeded`, and any
/// read of the private key.
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
    /// The pre-custody layout: the raw X25519 private key, 32 bytes.
    static let rawKeySize = 32

    /// The one the app uses: the Secure Enclave wrapper when this device
    /// can use one, the software wrapper otherwise (KeyWrapperFactory).
    static let app = VaultKeyStore(slots: KeychainSlots(service: service),
                                   preferred: KeyWrapperFactory.make(),
                                   wrappers: KeyWrapperFactory.all())

    let slots: SecretSlots
    /// The wrapper a NEW key (or a migrated one) is wrapped with.
    let preferred: KeyWrapper
    /// Every wrapper a stored record may name. Only the one it names opens it.
    let wrappers: [CustodyKind: KeyWrapper]

    init(slots: SecretSlots, preferred: KeyWrapper, wrappers: [CustodyKind: KeyWrapper]? = nil) {
        self.slots = slots
        self.preferred = preferred
        self.wrappers = wrappers ?? [preferred.custody: preferred]
    }

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
        // A pre-custody private item with no public twin can be repaired
        // here, with no prompt. A WRAPPED one needs an unwrap, which may ask
        // for Face ID — never from a property read; `generateIfNeeded`
        // repairs that case.
        guard let stored = slots.get(Self.account), stored.count == Self.rawKeySize,
              let key = try? Curve25519.KeyAgreement.PrivateKey(rawRepresentation: stored) else {
            return nil
        }
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

    /// Where the key's protection lives, read off the record's first byte —
    /// no unwrap, no prompt. Nil when there is no key, or while it is still
    /// in the pre-custody layout.
    var custody: CustodyKind? {
        guard let stored = slots.get(Self.account), stored.count != Self.rawKeySize,
              let tag = stored.first else { return nil }
        return CustodyKind(tag: tag)
    }

    /// Ask for the user's presence ONCE, asynchronously, when the key's
    /// custody needs it — and hand back the evaluated context, so the
    /// Secure Enclave uses it instead of prompting again (synchronously, on
    /// whatever thread did the unwrap). Nil when no presence is needed.
    func authenticate(reason: String) async throws -> LAContext? {
        guard custody == .secureEnclave else { return nil }
        let context = LAContext()
        _ = try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        return context
    }

    /// The private key, for an unseal. Nil when none exists; throws when the
    /// record cannot be opened (never a guess at a key). A pre-custody raw
    /// item is wrapped in place on the way out.
    func privateKey(context: LAContext? = nil) throws -> Curve25519.KeyAgreement.PrivateKey? {
        guard let stored = slots.get(Self.account) else { return nil }
        if stored.count == Self.rawKeySize {
            let key = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: stored)
            try? slots.set(record(wrapping: stored), Self.account)
            return key
        }
        guard let tag = stored.first, let kind = CustodyKind(tag: tag) else {
            throw CustodyError.malformedEnvelope
        }
        guard let wrapper = wrappers[kind] else { throw CustodyError.wrappingKeyMissing }
        let raw = try wrapper.unwrap(Data(stored.dropFirst()), context: context)
        return try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: raw)
    }

    /// Wrap a pre-custody raw key in place. True when it did. Wrapping needs
    /// only the wrapping key's public half, so this never prompts.
    @discardableResult
    func migrateIfNeeded() throws -> Bool {
        guard let stored = slots.get(Self.account), stored.count == Self.rawKeySize else {
            return false
        }
        _ = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: stored)
        try slots.set(record(wrapping: stored), Self.account)
        return true
    }

    private func record(wrapping raw: Data) throws -> Data {
        let envelope = try preferred.wrap(raw)
        return Data([preferred.custody.tag]) + envelope
    }

    /// Create the key if this phone has none; return its public half.
    /// All or nothing: a failed write removes what it wrote. A private key
    /// with no public twin is REPAIRED, never replaced — snapshots may
    /// already be sealed to it (that repair is the one path here that may
    /// ask for Face ID, because it has to unwrap).
    @discardableResult
    func generateIfNeeded() throws -> Curve25519.KeyAgreement.PublicKey {
        if let pub = publicKeyRaw {
            return try Curve25519.KeyAgreement.PublicKey(rawRepresentation: pub)
        }
        if let existing = try privateKey() {
            try slots.set(existing.publicKey.rawRepresentation, Self.publicAccount)
            return existing.publicKey
        }
        let key = Curve25519.KeyAgreement.PrivateKey()
        do {
            try slots.set(record(wrapping: key.rawRepresentation), Self.account)
            try slots.set(key.publicKey.rawRepresentation, Self.publicAccount)
        } catch {
            slots.delete(Self.account)
            slots.delete(Self.publicAccount)
            throw error
        }
        return key.publicKey
    }

    /// Delete the key, and the wrapping keys that protected it. The caller
    /// has shown `forgetWarning` and had it confirmed — this method does not
    /// ask, it does.
    func forget() {
        slots.delete(Self.account)
        slots.delete(Self.publicAccount)
        for wrapper in wrappers.values { wrapper.forget() }
    }
}
