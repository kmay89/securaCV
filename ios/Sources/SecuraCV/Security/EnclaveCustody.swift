// EnclaveCustody.swift
//
// Secure-Enclave-backed custody for the one secret on this phone whose
// exposure is media: the sealed-snapshot key (VaultKeyStore).
//
// What the Secure Enclave can and cannot do, stated once so no copy
// overclaims it: the Enclave generates and holds only P-256 keys. The
// snapshot key is X25519 (the Canary seals to it), so it can NEVER live in
// the Enclave, and a snapshot is never "decrypted inside the Secure
// Enclave" — the X25519 key is in app memory for the length of an unseal.
// What the Enclave does is WRAP it: an Enclave P-256 key, usable only after
// Face ID, Touch ID or the passcode (`.userPresence`), is the only thing that
// can unwrap the Keychain item holding the X25519 key. So the item alone —
// in a backup, on another device, read by other code — opens nothing.
//
// Envelope v1 (the same bytes whichever wrapper made it):
//
//   0x01 ‖ ephemeral P-256 public key (x963, 65) ‖ AES-GCM nonce (12)
//        ‖ ciphertext ‖ tag (16)
//
//   key = HKDF-SHA256(ikm = ECDH(wrapping key, ephemeral key),
//                     salt = ephemeral x963, info = "securacv/ios/custody/v1", 32)
//   AAD = the first 66 bytes (version ‖ ephemeral key)
//
// Wrapping needs only the wrapping key's PUBLIC half, so creating a key or
// migrating one never asks for Face ID; only an unwrap does.
//
// Both branches compile everywhere and the choice is a RUNTIME one
// (`SecureEnclave.isAvailable`), never `#if targetEnvironment(simulator)`:
// code behind an `#if` is code no CI compiler ever sees — the CloudKit
// lesson in .github/workflows/tvos.yml and ios/scripts/heal.sh. The simulator
// takes the software branch and its tests run; the Enclave branch is
// type-checked in CI and exercised only on a physical iPhone.

import CryptoKit
import Foundation
import LocalAuthentication
import Security

/// Where a wrapped key's wrapping key lives.
enum CustodyKind: String, CaseIterable, Sendable {
    case secureEnclave
    case software

    /// The byte a stored record starts with (VaultKeyStore), so the record
    /// itself names the wrapper that can open it.
    var tag: UInt8 {
        switch self {
        case .secureEnclave: return 1
        case .software: return 2
        }
    }

    init?(tag: UInt8) {
        guard let hit = Self.allCases.first(where: { $0.tag == tag }) else { return nil }
        self = hit
    }

    /// The Keys tab's one-row summary.
    var shortLabel: String {
        switch self {
        case .secureEnclave: return "Secure Enclave · Face ID, Touch ID or passcode to open"
        case .software: return "Software key · this device only"
        }
    }

    /// One honest line. "Wrapped by", never "decrypted in": see the header.
    var label: String {
        switch self {
        case .secureEnclave:
            return "Secure Enclave — the key is unwrapped through this device's Secure Enclave, "
                + "after Face ID, Touch ID or your passcode."
        case .software:
            return "Software key — wrapped by a second key in this device's Keychain, "
                + "device-only, with no Face ID step (there was no Secure Enclave, or no "
                + "passcode, when the key was made)."
        }
    }
}

enum CustodyError: Error, LocalizedError, Equatable {
    /// An envelope version this build does not read.
    case unsupportedEnvelope(UInt8)
    /// Too short, or an ephemeral key that is not a P-256 point.
    case malformedEnvelope
    /// The tag did not verify: a different wrapping key, or a changed blob.
    case unwrapFailed
    /// The wrapping key this blob needs is gone (forgotten, or this is not
    /// the device that wrapped it).
    case wrappingKeyMissing

    var errorDescription: String? {
        switch self {
        case .unsupportedEnvelope(let v):
            return "This phone's snapshot key is stored in a format (\(v)) this app doesn't read."
        case .malformedEnvelope:
            return "This phone's stored snapshot key is damaged."
        case .unwrapFailed:
            return "This phone's snapshot key couldn't be unwrapped — it was wrapped by a key "
                + "this device no longer holds, or it was changed."
        case .wrappingKeyMissing:
            return "The key that protects this phone's snapshot key is gone, so the snapshot "
                + "key can't be opened."
        }
    }
}

/// The v1 envelope, shared by both wrappers. The ECDH is the wrapper's own
/// (it may need the Secure Enclave); everything around it is here, once.
enum CustodyEnvelope {
    static let version: UInt8 = 0x01
    static let info = Data("securacv/ios/custody/v1".utf8)
    static let ephemeralSize = 65
    static let nonceSize = 12
    static let tagSize = 16
    /// version ‖ ephemeral key — the authenticated header.
    static let headerSize = 1 + ephemeralSize
    static let minSize = headerSize + nonceSize + tagSize

    struct Parsed {
        let header: Data
        let ephemeral: P256.KeyAgreement.PublicKey
        let box: AES.GCM.SealedBox
    }

    /// Seal `plain` to a wrapping key's public half. The ephemeral key and
    /// nonce are injectable for tests; callers pass nothing.
    static func seal(_ plain: Data,
                     to recipient: P256.KeyAgreement.PublicKey,
                     ephemeral: P256.KeyAgreement.PrivateKey = .init(),
                     nonce: AES.GCM.Nonce = AES.GCM.Nonce()) throws -> Data {
        let ephemeralPub = ephemeral.publicKey.x963Representation
        let shared = try ephemeral.sharedSecretFromKeyAgreement(with: recipient)
        var header = Data([version])
        header.append(ephemeralPub)
        let box = try AES.GCM.seal(plain, using: symmetricKey(shared, ephemeralPub: ephemeralPub),
                                   nonce: nonce, authenticating: header)
        return header + Data(nonce) + box.ciphertext + box.tag
    }

    static func parse(_ blob: Data) throws -> Parsed {
        let b = [UInt8](blob)
        guard b.count >= minSize else { throw CustodyError.malformedEnvelope }
        guard b[0] == version else { throw CustodyError.unsupportedEnvelope(b[0]) }
        let ephemeral: P256.KeyAgreement.PublicKey
        do {
            ephemeral = try P256.KeyAgreement.PublicKey(x963Representation: Data(b[1..<headerSize]))
        } catch {
            throw CustodyError.malformedEnvelope
        }
        let nonceEnd = headerSize + nonceSize
        let tagStart = b.count - tagSize
        let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: Data(b[headerSize..<nonceEnd])),
                                        ciphertext: Data(b[nonceEnd..<tagStart]),
                                        tag: Data(b[tagStart..<b.count]))
        return Parsed(header: Data(b[0..<headerSize]), ephemeral: ephemeral, box: box)
    }

    static func open(_ parsed: Parsed, shared: SharedSecret) throws -> Data {
        let key = symmetricKey(shared, ephemeralPub: parsed.ephemeral.x963Representation)
        do {
            return try AES.GCM.open(parsed.box, using: key, authenticating: parsed.header)
        } catch {
            throw CustodyError.unwrapFailed
        }
    }

    static func symmetricKey(_ shared: SharedSecret, ephemeralPub: Data) -> SymmetricKey {
        shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: ephemeralPub,
                                       sharedInfo: info, outputByteCount: 32)
    }
}

/// Wraps and unwraps small secrets under a P-256 key agreement key.
protocol KeyWrapper {
    var custody: CustodyKind { get }
    func wrap(_ plain: Data) throws -> Data
    /// `context`: an `LAContext` that has ALREADY evaluated
    /// `.deviceOwnerAuthentication`, so a presence-gated key is used without
    /// a second prompt (and without a synchronous one on the caller's
    /// thread). Ignored by a wrapper whose key needs no presence.
    func unwrap(_ blob: Data, context: LAContext?) throws -> Data
    /// Delete the wrapping key. Every blob it wrapped becomes unreadable.
    func forget()
}

/// The Enclave branch. The P-256 key is generated IN the Secure Enclave with
/// `.privateKeyUsage` + `.userPresence` and `WhenUnlockedThisDeviceOnly`;
/// what the Keychain holds (`dataRepresentation`) is an opaque handle only
/// this device's Enclave can use — not the key.
///
/// `.userPresence` (Face ID / Touch ID OR the passcode), not
/// `.biometryCurrentSet`: re-enrolling a face or a finger would silently and
/// permanently destroy a `.biometryCurrentSet` key, and every snapshot with
/// it. Removing the device passcode does disable the key; the factory only
/// picks this branch when a passcode is set, and the docs say so.
struct SecureEnclaveWrapper: KeyWrapper {
    static let account = "wrapping-p256-v1"
    let slots: SecretSlots

    var custody: CustodyKind { .secureEnclave }

    /// The existing Enclave key, or a new one. Creating it asks for nothing.
    private func key(context: LAContext?) throws -> SecureEnclave.P256.KeyAgreement.PrivateKey {
        if let handle = slots.get(Self.account) {
            return try SecureEnclave.P256.KeyAgreement.PrivateKey(dataRepresentation: handle,
                                                                   authenticationContext: context)
        }
        var error: Unmanaged<CFError>?
        guard let access = SecAccessControlCreateWithFlags(
            nil, kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
            [.privateKeyUsage, .userPresence], &error) else {
            if let cfError = error?.takeRetainedValue() { throw cfError }
            throw CustodyError.wrappingKeyMissing
        }
        let key = try SecureEnclave.P256.KeyAgreement.PrivateKey(accessControl: access,
                                                                 authenticationContext: context)
        try slots.set(key.dataRepresentation, Self.account)
        return key
    }

    func wrap(_ plain: Data) throws -> Data {
        // The public half needs no presence: wrapping never prompts.
        try CustodyEnvelope.seal(plain, to: key(context: nil).publicKey)
    }

    func unwrap(_ blob: Data, context: LAContext?) throws -> Data {
        let parsed = try CustodyEnvelope.parse(blob)
        guard let handle = slots.get(Self.account) else { throw CustodyError.wrappingKeyMissing }
        let key = try SecureEnclave.P256.KeyAgreement.PrivateKey(dataRepresentation: handle,
                                                                 authenticationContext: context)
        // THIS is the Enclave operation that needs the user's presence.
        let shared = try key.sharedSecretFromKeyAgreement(with: parsed.ephemeral)
        return try CustodyEnvelope.open(parsed, shared: shared)
    }

    func forget() { slots.delete(Self.account) }
}

/// The software branch: the SAME envelope under a P-256 key kept in the
/// Keychain (device-only). Honest about what it is — no hardware binding,
/// no presence gate — and labeled so; it is what a device without a
/// Secure Enclave (or without a passcode, or the simulator) gets.
struct SoftwareWrapper: KeyWrapper {
    static let account = "wrapping-p256-v1"
    let slots: SecretSlots

    var custody: CustodyKind { .software }

    private func existingKey() throws -> P256.KeyAgreement.PrivateKey? {
        guard let raw = slots.get(Self.account) else { return nil }
        return try P256.KeyAgreement.PrivateKey(rawRepresentation: raw)
    }

    func wrap(_ plain: Data) throws -> Data {
        let key: P256.KeyAgreement.PrivateKey
        if let existing = try existingKey() {
            key = existing
        } else {
            key = P256.KeyAgreement.PrivateKey()
            try slots.set(key.rawRepresentation, Self.account)
        }
        return try CustodyEnvelope.seal(plain, to: key.publicKey)
    }

    func unwrap(_ blob: Data, context: LAContext?) throws -> Data {
        let parsed = try CustodyEnvelope.parse(blob)
        guard let key = try existingKey() else { throw CustodyError.wrappingKeyMissing }
        let shared = try key.sharedSecretFromKeyAgreement(with: parsed.ephemeral)
        return try CustodyEnvelope.open(parsed, shared: shared)
    }

    func forget() { slots.delete(Self.account) }
}

/// Which wrapper a NEW key gets, decided at runtime.
enum KeyWrapperFactory {
    static let enclaveService = "com.securacv.custody.se"
    static let softwareService = "com.securacv.custody.sw"

    /// The Secure Enclave when this device has one AND a passcode is set
    /// (a `.userPresence` key on a device with no passcode could never be
    /// used — it would lock every snapshot away); the software wrapper
    /// otherwise. Both inputs are parameters so the choice is testable on
    /// a simulator that has neither.
    static func make(secureEnclaveAvailable: Bool = SecureEnclave.isAvailable,
                     ownerAuthenticationAvailable: Bool = LAContext()
                        .canEvaluatePolicy(.deviceOwnerAuthentication, error: nil)) -> KeyWrapper {
        wrapper(for: secureEnclaveAvailable && ownerAuthenticationAvailable
                ? .secureEnclave : .software)
    }

    static func wrapper(for kind: CustodyKind) -> KeyWrapper {
        switch kind {
        case .secureEnclave: return SecureEnclaveWrapper(slots: KeychainSlots(service: enclaveService))
        case .software: return SoftwareWrapper(slots: KeychainSlots(service: softwareService))
        }
    }

    /// Every wrapper, by kind — a stored key names the one that opens it.
    static func all() -> [CustodyKind: KeyWrapper] {
        Dictionary(uniqueKeysWithValues: CustodyKind.allCases.map { ($0, wrapper(for: $0)) })
    }
}
