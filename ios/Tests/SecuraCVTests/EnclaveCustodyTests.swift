// EnclaveCustodyTests.swift
//
// The custody wrapper around the sealed-snapshot key (EnclaveCustody.swift).
// Everything here runs on the simulator, which has no Secure Enclave — so
// this suite proves the envelope, the software wrapper, the store's
// migration and routing, and that the factory CHOOSES software there. The
// Secure Enclave branch is compiled by the same build (a runtime branch,
// never an `#if`) but only a physical iPhone can run it: that is a human
// pass, recorded as such, not something a green run here may be read as.

import CryptoKit
import LocalAuthentication
import XCTest
@testable import SecuraCV

final class EnclaveCustodyTests: XCTestCase {

    private let secret = Data((0..<32).map { UInt8($0 ^ 0x5A) })

    // MARK: - the software wrapper + envelope v1

    func testSoftwareWrapRoundTrips() throws {
        let wrapper = SoftwareWrapper(slots: MemorySlots())
        let blob = try wrapper.wrap(secret)
        XCTAssertEqual(try wrapper.unwrap(blob, context: nil), secret)
        XCTAssertEqual(wrapper.custody, .software)
    }

    func testEnvelopeShapeIsV1() throws {
        let blob = try SoftwareWrapper(slots: MemorySlots()).wrap(secret)
        XCTAssertEqual(blob.count, 1 + 65 + 12 + secret.count + 16,
                       "0x01 ‖ x963 ephemeral ‖ nonce ‖ ciphertext ‖ tag")
        XCTAssertEqual(blob[blob.startIndex], 0x01, "version byte")
        XCTAssertEqual(blob[blob.startIndex + 1], 0x04, "an uncompressed (x963) P-256 point")
        XCTAssertNil(blob.range(of: secret), "the wrapped secret must not appear in the clear")
    }

    func testEveryWrapIsFresh() throws {
        let wrapper = SoftwareWrapper(slots: MemorySlots())
        let a = try wrapper.wrap(secret)
        let b = try wrapper.wrap(secret)
        XCTAssertNotEqual(a, b, "a fresh ephemeral key and nonce per wrap")
        XCTAssertEqual(try wrapper.unwrap(a, context: nil), secret)
        XCTAssertEqual(try wrapper.unwrap(b, context: nil), secret)
    }

    func testTheEnvelopeIsDeterministicGivenItsRandomness() throws {
        let recipient = P256.KeyAgreement.PrivateKey()
        let ephemeral = P256.KeyAgreement.PrivateKey()
        let nonce = try AES.GCM.Nonce(data: Data(repeating: 7, count: 12))
        let a = try CustodyEnvelope.seal(secret, to: recipient.publicKey, ephemeral: ephemeral, nonce: nonce)
        let b = try CustodyEnvelope.seal(secret, to: recipient.publicKey, ephemeral: ephemeral, nonce: nonce)
        XCTAssertEqual(a, b)
        let parsed = try CustodyEnvelope.parse(a)
        let shared = try recipient.sharedSecretFromKeyAgreement(with: parsed.ephemeral)
        XCTAssertEqual(try CustodyEnvelope.open(parsed, shared: shared), secret)
    }

    func testAnUnknownVersionOrAShortBlobIsRefused() throws {
        let wrapper = SoftwareWrapper(slots: MemorySlots())
        var raw = [UInt8](try wrapper.wrap(secret))
        raw[0] = 0x02
        XCTAssertThrowsError(try wrapper.unwrap(Data(raw), context: nil)) {
            XCTAssertEqual($0 as? CustodyError, .unsupportedEnvelope(2))
        }
        XCTAssertThrowsError(try wrapper.unwrap(Data(repeating: 1, count: CustodyEnvelope.minSize - 1),
                                                context: nil)) {
            XCTAssertEqual($0 as? CustodyError, .malformedEnvelope)
        }
    }

    func testTamperingFailsTheTag() throws {
        let wrapper = SoftwareWrapper(slots: MemorySlots())
        let blob = try wrapper.wrap(secret)
        for (what, index) in [("nonce", 1 + 65 + 2),
                              ("ciphertext", 1 + 65 + 12 + 5),
                              ("tag", blob.count - 1)] {
            var raw = [UInt8](blob)
            raw[index] ^= 0x01
            XCTAssertThrowsError(try wrapper.unwrap(Data(raw), context: nil), what) {
                XCTAssertEqual($0 as? CustodyError, .unwrapFailed, what)
            }
        }
        // The ephemeral key is authenticated too (AAD and HKDF salt); a
        // flipped coordinate is either off the curve or the wrong key.
        var raw = [UInt8](blob)
        raw[10] ^= 0x01
        XCTAssertThrowsError(try wrapper.unwrap(Data(raw), context: nil), "ephemeral key")
    }

    func testAnotherWrappingKeyCannotOpenIt() throws {
        let blob = try SoftwareWrapper(slots: MemorySlots()).wrap(secret)
        let other = SoftwareWrapper(slots: MemorySlots())
        _ = try other.wrap(Data([1]))   // give it a wrapping key of its own
        XCTAssertThrowsError(try other.unwrap(blob, context: nil)) {
            XCTAssertEqual($0 as? CustodyError, .unwrapFailed)
        }
    }

    func testAForgottenWrappingKeyOpensNothing() throws {
        let slots = MemorySlots()
        let wrapper = SoftwareWrapper(slots: slots)
        let blob = try wrapper.wrap(secret)
        wrapper.forget()
        XCTAssertTrue(slots.items.isEmpty)
        XCTAssertThrowsError(try wrapper.unwrap(blob, context: nil)) {
            XCTAssertEqual($0 as? CustodyError, .wrappingKeyMissing)
        }
    }

    // MARK: - the factory (runtime choice, no #if)

    func testTheFactoryChoosesTheEnclaveOnlyWithAPasscodeToGateIt() {
        XCTAssertEqual(KeyWrapperFactory.make(secureEnclaveAvailable: true,
                                              ownerAuthenticationAvailable: true).custody,
                       .secureEnclave)
        XCTAssertEqual(KeyWrapperFactory.make(secureEnclaveAvailable: true,
                                              ownerAuthenticationAvailable: false).custody,
                       .software, "a presence-gated key with no passcode could never be used")
        XCTAssertEqual(KeyWrapperFactory.make(secureEnclaveAvailable: false,
                                              ownerAuthenticationAvailable: true).custody,
                       .software)
        XCTAssertEqual(Set(KeyWrapperFactory.all().keys), Set(CustodyKind.allCases))
    }

    func testThisHostGetsTheWrapperItCanActuallyUse() {
        // On the CI simulator SecureEnclave.isAvailable is false: the app's
        // real factory must pick software there, and this asserts it does.
        if !SecureEnclave.isAvailable {
            XCTAssertEqual(KeyWrapperFactory.make().custody, .software)
        }
    }

    // MARK: - honest labels

    func testCustodyCopyNeverOverclaims() {
        for kind in CustodyKind.allCases {
            let copy = (kind.label + " " + kind.shortLabel).lowercased()
            XCTAssertFalse(copy.contains("decrypted in"), "\(kind): the X25519 key is never in the Enclave")
            XCTAssertFalse(copy.contains("inside the secure enclave"), "\(kind)")
            XCTAssertEqual(CustodyKind(tag: kind.tag), kind)
        }
        XCTAssertTrue(CustodyKind.secureEnclave.label.contains("Secure Enclave"))
        XCTAssertFalse(CustodyKind.software.shortLabel.contains("Secure Enclave"),
                       "a software key must not borrow the Enclave's name")
        XCTAssertNil(CustodyKind(tag: 0))
    }

    @MainActor
    func testTheKeysTabNamesTheCustodyItHas() throws {
        let keys = memoryVaultKeys()
        XCTAssertNil(KeysView.custodyNote(keys), "no key, no custody row")
        try keys.generateIfNeeded()
        XCTAssertEqual(KeysView.custodyNote(keys), CustodyKind.software.shortLabel)
    }

    // MARK: - VaultKeyStore: records, routing, migration

    func testANewKeyIsStoredWrappedNeverRaw() throws {
        let slots = MemorySlots()
        let store = memoryVaultKeys(slots)
        let pub = try store.generateIfNeeded()
        let record = try XCTUnwrap(slots.items[VaultKeyStore.account])
        XCTAssertEqual(record.first, CustodyKind.software.tag, "the record names its wrapper")
        XCTAssertNotEqual(record.count, VaultKeyStore.rawKeySize)
        let key = try XCTUnwrap(try store.privateKey())
        XCTAssertEqual(key.publicKey.rawRepresentation, pub.rawRepresentation)
        XCTAssertNil(record.range(of: key.rawRepresentation), "the raw X25519 key is not in the item")
        XCTAssertEqual(store.custody, .software)
    }

    func testARawKeyIsWrappedInPlaceAndKeepsItsIdentity() throws {
        let slots = MemorySlots()
        let legacy = Curve25519.KeyAgreement.PrivateKey()
        slots.items[VaultKeyStore.account] = legacy.rawRepresentation
        slots.items[VaultKeyStore.publicAccount] = legacy.publicKey.rawRepresentation
        let store = memoryVaultKeys(slots)

        XCTAssertNil(store.custody, "not wrapped yet")
        let idBefore = store.keyIDHex
        XCTAssertTrue(try store.migrateIfNeeded())
        XCTAssertFalse(try store.migrateIfNeeded(), "once")
        XCTAssertEqual(store.custody, .software)
        XCTAssertNotEqual(slots.items[VaultKeyStore.account], legacy.rawRepresentation)
        XCTAssertEqual(store.keyIDHex, idBefore, "same key, same id — every sealed file still opens")
        XCTAssertEqual(try store.privateKey()?.rawRepresentation, legacy.rawRepresentation)
    }

    func testAFailedWrapWriteKeepsTheRawKey() throws {
        let slots = MemorySlots()
        let legacy = Curve25519.KeyAgreement.PrivateKey()
        slots.items[VaultKeyStore.account] = legacy.rawRepresentation
        slots.items[VaultKeyStore.publicAccount] = legacy.publicKey.rawRepresentation
        let store = memoryVaultKeys(slots)
        slots.refusing = [VaultKeyStore.account]

        XCTAssertThrowsError(try store.migrateIfNeeded(), "the refused write surfaces")
        XCTAssertEqual(slots.items[VaultKeyStore.account], legacy.rawRepresentation,
                       "a failed migration leaves the raw key, never nothing")
        XCTAssertEqual(try store.privateKey()?.rawRepresentation, legacy.rawRepresentation,
                       "and the key still opens (the read's best-effort wrap failed quietly)")
        XCTAssertNil(store.custody, "still the pre-custody layout")

        slots.refusing = []
        XCTAssertTrue(try store.migrateIfNeeded(), "the next attempt wraps it")
        XCTAssertEqual(try store.privateKey()?.rawRepresentation, legacy.rawRepresentation)
    }

    func testTheFirstReadOfARawKeyAlsoWrapsIt() throws {
        let slots = MemorySlots()
        let legacy = Curve25519.KeyAgreement.PrivateKey()
        slots.items[VaultKeyStore.account] = legacy.rawRepresentation
        let store = memoryVaultKeys(slots)
        XCTAssertEqual(try store.privateKey()?.rawRepresentation, legacy.rawRepresentation)
        XCTAssertEqual(store.custody, .software)
        XCTAssertEqual(try store.privateKey()?.rawRepresentation, legacy.rawRepresentation)
    }

    func testARecordOpensOnlyThroughTheWrapperItNames() throws {
        let slots = MemorySlots()
        try memoryVaultKeys(slots).generateIfNeeded()
        // Relabel the record as an Enclave one: a store that has no Enclave
        // wrapper must refuse, not try the software key on it.
        var record = [UInt8](try XCTUnwrap(slots.items[VaultKeyStore.account]))
        record[0] = CustodyKind.secureEnclave.tag
        slots.items[VaultKeyStore.account] = Data(record)
        XCTAssertThrowsError(try memoryVaultKeys(slots).privateKey()) {
            XCTAssertEqual($0 as? CustodyError, .wrappingKeyMissing)
        }
    }

    func testAWrappedKeyWithoutItsPublicTwinIsRepairedNotReplaced() throws {
        let slots = MemorySlots()
        let wrapperSlots = MemorySlots()
        let store = memoryVaultKeys(slots, wrapperSlots: wrapperSlots)
        let pub = try store.generateIfNeeded()
        slots.items[VaultKeyStore.publicAccount] = nil
        XCTAssertNil(store.publicKeyRaw, "a property read never unwraps")
        XCTAssertEqual(try store.generateIfNeeded().rawRepresentation, pub.rawRepresentation,
                       "snapshots may be sealed to it — never silently replaced")
        XCTAssertEqual(store.publicKeyRaw, pub.rawRepresentation)
    }

    func testPresenceIsAskedOnlyForAnEnclaveKey() async throws {
        // Software custody needs no LAContext; the Enclave branch is the
        // device-only human pass.
        let store = memoryVaultKeys()
        try store.generateIfNeeded()
        let context = try await store.authenticate(reason: "test")
        XCTAssertNil(context)
    }
}
