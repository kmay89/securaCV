// UnsealFlowTests.swift
//
// Everything around the sealed-snapshot crypto (SnapshotVaultTests owns the
// format itself):
//
//   * DeviceAPI's /api/vault/* routes, driven through a stub URLProtocol so
//     the real request building, auth and error mapping run — including the
//     two 404s that mean different things (no such route = firmware without
//     sealed snapshots; no such file = rotated out of the ring), and the
//     filename gate that must refuse BEFORE anything is sent;
//   * VaultKeyStore over in-memory slots — never the simulator Keychain,
//     whose availability to an unsigned CI build is the host's business;
//   * UnsealModel's one decrypt path and its discard exits.

import CryptoKit
import XCTest
@testable import SecuraCV

/// A dictionary standing in for the Keychain (see `SecretSlots`).
final class MemorySlots: SecretSlots {
    var items: [String: Data] = [:]
    func get(_ account: String) -> Data? { items[account] }
    func set(_ data: Data, _ account: String) throws { items[account] = data }
    func delete(_ account: String) { items[account] = nil }
}

/// Answers every request from `respond` and records what was asked.
final class VaultStubProtocol: URLProtocol {
    struct Seen {
        let method: String
        let url: URL
        let authorization: String?
    }

    static var seen: [Seen] = []
    static var respond: (URLRequest) -> (Int, Data) = { _ in (500, Data()) }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        let (status, body) = Self.respond(request)
        guard let url = request.url,
              let response = HTTPURLResponse(url: url, statusCode: status,
                                             httpVersion: "HTTP/1.1", headerFields: nil) else {
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
            return
        }
        Self.seen.append(Seen(method: request.httpMethod ?? "GET", url: url,
                              authorization: request.value(forHTTPHeaderField: "Authorization")))
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: body)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

final class UnsealFlowTests: XCTestCase {

    private let good = "seal_00000007_smoke.svlt"

    override func setUp() {
        super.setUp()
        VaultStubProtocol.seen = []
        VaultStubProtocol.respond = { _ in (500, Data()) }
    }

    private func api() throws -> DeviceAPI {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [VaultStubProtocol.self]
        return try DeviceAPI(base: URL(string: "http://192.168.1.20")!, token: "t0k3n",
                             session: URLSession(configuration: config))
    }

    private func answer(_ status: Int, _ json: String) {
        VaultStubProtocol.respond = { _ in (status, Data(json.utf8)) }
    }

    // MARK: - the routes

    func testStatusReadsTheFirmwareBodyWithTheBearerToken() async throws {
        // handle_vault_status's body, verbatim shape.
        answer(200, #"{"ok":true,"has_key":true,"key_id":"0102030405060708","t3_smoke":false,"#
               + #""t4_co":false,"glass":true,"motion":false,"mesh":false,"cooldown_s":60,"#
               + #""sealing":false,"sd_ok":true,"camera_ok":true,"keep_files":20}"#)
        let status = try await api().vaultStatus()
        XCTAssertEqual(status.hasKey, true)
        XCTAssertEqual(status.keyID, "0102030405060708")
        XCTAssertEqual(status.sdOK, true)
        XCTAssertEqual(status.keepFiles, 20)
        let seen = try XCTUnwrap(VaultStubProtocol.seen.last)
        XCTAssertEqual(seen.method, "GET")
        XCTAssertEqual(seen.url.path, "/api/vault/status")
        XCTAssertEqual(seen.authorization, "Bearer t0k3n", "the vault routes read Bearer")
    }

    func testAMissingRouteIsFirmwareWithoutSealedSnapshots() async throws {
        answer(404, "Nothing matches the given URI")
        do {
            _ = try await api().vaultList()
            XCTFail("a 404 on the list route must throw")
        } catch DeviceError.noSealedSnapshots {
            // the honest "not here"
        }
    }

    func testAMissingFileIsGoneNotAnOldFirmware() async throws {
        // handle_vault_download's own 404: the route answers, the file left the ring.
        answer(404, #"{"ok":false,"error":"No such sealed file"}"#)
        do {
            _ = try await api().vaultDownload(name: good)
            XCTFail("a 404 on a named file must throw")
        } catch DeviceError.sealedSnapshotGone(let name) {
            XCTAssertEqual(name, good)
        }
        do {
            try await api().vaultDelete(name: good)
            XCTFail("a 404 on a named delete must throw")
        } catch DeviceError.sealedSnapshotGone(let name) {
            XCTAssertEqual(name, good)
        }
    }

    func testABadNameIsRefusedBeforeAnythingIsSent() async throws {
        let client = try api()
        for bad in ["../VAULT/seal_00000007_smoke.svlt", "seal_7_smoke.svlt", "x.svlt", ""] {
            do {
                _ = try await client.vaultDownload(name: bad)
                XCTFail("\(bad) reached the network")
            } catch DeviceError.badVaultFilename(let refused) {
                XCTAssertEqual(refused, bad)
            }
            do {
                try await client.vaultDelete(name: bad)
                XCTFail("\(bad) reached the network")
            } catch DeviceError.badVaultFilename(let refused) {
                XCTAssertEqual(refused, bad)
            }
        }
        XCTAssertTrue(VaultStubProtocol.seen.isEmpty, "a refused name must never become a request")
    }

    func testDownloadAndDeleteCarryTheNameAsAQuery() async throws {
        VaultStubProtocol.respond = { _ in (200, Data([0x53, 0x56, 0x4C, 0x54])) }
        let client = try api()
        let bytes = try await client.vaultDownload(name: good)
        XCTAssertEqual(bytes, Data([0x53, 0x56, 0x4C, 0x54]))
        answer(200, #"{"ok":true}"#)
        try await client.vaultDelete(name: good)
        answer(200, #"{"ok":true,"has_key":false}"#)
        try await client.vaultDeleteKey()

        let seen = VaultStubProtocol.seen
        XCTAssertEqual(seen.map(\.method), ["GET", "DELETE", "DELETE"])
        XCTAssertEqual(seen.map(\.url.path), ["/api/vault/download", "/api/vault/item", "/api/vault/key"])
        XCTAssertEqual(seen[0].url.query, "name=\(good)")
        XCTAssertEqual(seen[1].url.query, "name=\(good)")
        XCTAssertNil(seen[2].url.query)
    }

    func testRegisteringAKeyAnswersTheDeviceKeyIDOrThrowsItsRefusal() async throws {
        let hex = String(repeating: "ab", count: 32)
        answer(200, #"{"ok":true,"key_id":"a1b2c3d4e5f60718"}"#)
        let id = try await api().vaultRegisterKey(hex: hex)
        XCTAssertEqual(id, "a1b2c3d4e5f60718")
        XCTAssertEqual(VaultStubProtocol.seen.last?.method, "POST")
        XCTAssertEqual(VaultStubProtocol.seen.last?.url.path, "/api/vault/key")

        // The firmware refuses on a 200 (the /api/wifi/connect shape).
        answer(200, #"{"ok":false,"error":"pubkey must be the 64-hex X25519 public key"}"#)
        do {
            _ = try await api().vaultRegisterKey(hex: hex)
            XCTFail("an ok:false answer is a refusal, not a success")
        } catch DeviceError.http(200, let message) {
            XCTAssertTrue(message.contains("64-hex"))
        }
    }

    // MARK: - the pure edges

    func testKeyBodyIsExactlyWhatTheFirmwareParses() throws {
        let upper = "  " + String(repeating: "AB", count: 32) + "\n"
        let body = try DeviceAPI.vaultKeyBody(hex: upper)
        let obj = try XCTUnwrap(JSONSerialization.jsonObject(with: body) as? [String: String])
        XCTAssertEqual(obj, ["pubkey": String(repeating: "ab", count: 32)])

        for bad in [String(repeating: "a", count: 63), String(repeating: "g", count: 64),
                    String(repeating: "１", count: 64)] {   // fullwidth digits: isHexDigit alone says yes
            XCTAssertThrowsError(try DeviceAPI.vaultKeyBody(hex: bad), bad) {
                guard let error = $0 as? DeviceError, case .badPublicKeyHex = error else {
                    return XCTFail("wrong error for \(bad): \($0)")
                }
            }
        }
    }

    func testListKeepsGoodRowsAndDropsBadOnesOneByOne() throws {
        let json = #"""
        {"ok":true,"sd_ok":true,"items":[
          {"name":"seal_00000007_smoke.svlt","trigger":"smoke","time_bucket":87,"size":131072},
          {"name":"../../etc/passwd","trigger":"smoke","time_bucket":1,"size":1},
          {"name":"seal_00000008_glass.svlt","trigger":"glass","time_bucket":"late","size":2},
          {"trigger":"co","time_bucket":3,"size":3},
          {"name":"seal_00000009_test.svlt","trigger":"test","time_bucket":255,"size":4}
        ]}
        """#
        let list = try DeviceAPI.decoder.decode(VaultList.self, from: Data(json.utf8))
        XCTAssertEqual(list.items.map(\.name), ["seal_00000007_smoke.svlt", "seal_00000009_test.svlt"],
                       "one malformed row must cost that row, never the list")
        XCTAssertEqual(list.items[0].svltTrigger, .smoke)
        XCTAssertEqual(list.items[0].bucketLabel, "14:30 – 14:40-ish")
        XCTAssertEqual(list.items[1].bucketLabel, "time unknown",
                       "255 is the firmware's 'header unreadable', not 23:50")
    }

    // MARK: - VaultKeyStore

    func testNoKeyUntilOneIsCreated() throws {
        let store = memoryVaultKeys()
        XCTAssertFalse(store.exists)
        XCTAssertNil(store.publicKeyHex)
        XCTAssertNil(store.keyIDHex)
        XCTAssertNil(store.custody)
        XCTAssertNil(try store.privateKey())
    }

    func testCreatedOnceThenKept() throws {
        let slots = MemorySlots()
        let store = memoryVaultKeys(slots)
        let first = try store.generateIfNeeded()
        let again = try store.generateIfNeeded()
        XCTAssertEqual(first.rawRepresentation, again.rawRepresentation, "never a silent second key")
        let pub = first.rawRepresentation
        XCTAssertEqual(store.publicKeyHex, ChainVerifier.hex(pub))
        XCTAssertEqual(store.publicKeyHex?.count, 64)
        XCTAssertEqual(store.keyIDHex, ChainVerifier.hex(Data(SHA256.hash(data: pub).prefix(8))))
        XCTAssertEqual(try store.privateKey()?.publicKey.rawRepresentation, pub)
        XCTAssertEqual(slots.items.count, 2, "the private key's record and its public twin, nothing else")
    }

    func testForgetRemovesEverything() throws {
        let slots = MemorySlots()
        let wrapperSlots = MemorySlots()
        let store = memoryVaultKeys(slots, wrapperSlots: wrapperSlots)
        try store.generateIfNeeded()
        XCTAssertFalse(wrapperSlots.items.isEmpty)
        store.forget()
        XCTAssertTrue(slots.items.isEmpty)
        XCTAssertTrue(wrapperSlots.items.isEmpty, "the wrapping key goes with the key it wrapped")
        XCTAssertFalse(store.exists)
    }

    func testAPrivateKeyWithoutItsPublicTwinIsRepaired() throws {
        let slots = MemorySlots()
        let key = Curve25519.KeyAgreement.PrivateKey()
        slots.items[VaultKeyStore.account] = key.rawRepresentation
        let store = memoryVaultKeys(slots)
        XCTAssertEqual(store.publicKeyRaw, key.publicKey.rawRepresentation)
        XCTAssertEqual(slots.items[VaultKeyStore.publicAccount], key.publicKey.rawRepresentation)
    }

    func testStoredBytesThatAreNotAKeyAreNeverOne() {
        let slots = MemorySlots()
        slots.items[VaultKeyStore.account] = Data(count: 5)
        XCTAssertThrowsError(try memoryVaultKeys(slots).privateKey())
    }

    // MARK: - UnsealModel

    @MainActor
    func testTheModelOpensOnlyItsOwnKeysFilesAndEveryExitDiscards() async throws {
        let keys = memoryVaultKeys()
        let model = UnsealModel(keys: keys)
        let jpeg = Data("not really a jpeg".utf8)

        // No key yet: nothing opens, and it says why.
        model.load()
        XCTAssertNil(model.keyIDHex)
        let stray = try SnapshotSealer.seal(plain: jpeg,
                                            recipient: Curve25519.KeyAgreement.PrivateKey().publicKey,
                                            trigger: .test, bucket: 3)
        await model.unseal(data: stray)
        XCTAssertNil(model.frame)
        XCTAssertNotNil(model.problem)
        model.problem = nil

        model.createKey()
        XCTAssertEqual(model.keyIDHex, keys.keyIDHex)
        XCTAssertEqual(model.custody, .software, "the simulator-safe wrapper these tests inject")
        let key = try XCTUnwrap(try keys.privateKey())
        let ours = try SnapshotSealer.seal(plain: jpeg, recipient: key.publicKey,
                                           trigger: .motion, bucket: 3)

        await model.unseal(data: ours)
        XCTAssertEqual(model.frame?.jpeg, jpeg)
        XCTAssertEqual(model.frame?.header.trigger, .motion)
        XCTAssertNil(model.problem)
        model.discard()
        XCTAssertNil(model.frame, "Done / background / disappear all come here")

        // A file sealed to another key: refused before any unwrap (so on a
        // device, before any Face ID prompt), with a reason naming both ids.
        let ourID = try XCTUnwrap(keys.keyIDHex)
        await model.unseal(data: stray)
        XCTAssertNil(model.frame)
        XCTAssertEqual(model.problem?.contains(ourID), true)
        model.problem = nil

        // Forgetting the key takes an open frame with it.
        await model.unseal(data: ours)
        XCTAssertNotNil(model.frame)
        model.forgetKey()
        XCTAssertNil(model.frame)
        XCTAssertNil(model.keyIDHex)
        XCTAssertFalse(keys.exists)
    }
}

/// A VaultKeyStore that never touches the Keychain or the Secure Enclave:
/// in-memory slots, wrapped by the software wrapper over its own in-memory
/// slots — the branch the simulator takes (EnclaveCustodyTests).
func memoryVaultKeys(_ slots: MemorySlots = MemorySlots(),
                     wrapperSlots: MemorySlots = MemorySlots()) -> VaultKeyStore {
    VaultKeyStore(slots: slots, preferred: SoftwareWrapper(slots: wrapperSlots))
}
