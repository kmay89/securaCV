// SnapshotVaultTests.swift
//
// The Swift third of the sealed-snapshot (.svlt) format's cross-language
// proof. The same 64-byte golden header is held verbatim by
// firmware/projects/canary-wap/tests_host/test_vault_logic.cpp (C++) and
// tools/test_unseal_snapshot.py (python); tools/fixtures/vault/svlt_parity.json
// (tools/gen_svlt_parity.py, from the python reference and FIXED test-only
// keys) pins one whole sealed file byte for byte. This suite opens that file
// with CryptoKit and re-seals it to the identical bytes: a construction that
// differs anywhere — salt order, info string, the header as AAD — fails at
// the tag, so the only way to pass is to BE the format.
//
// iOS CI (ios-selfheal.yml "selfheal") is the compiler and the runner; the
// python half and the generator's --check run in firmware.yml's
// "Mesh + Scout Host Tests". The fixture path follows the repo-relative
// #filePath idiom the other fixture-reading suites use.

import CryptoKit
import XCTest
@testable import SecuraCV

final class SnapshotVaultTests: XCTestCase {

    /// Verbatim from tools/test_unseal_snapshot.py (and test_vault_logic.cpp):
    /// trigger=1 smoke, bucket=87, key_id=01..08, ephemeral=A0..BF,
    /// nonce=C0..CB, ct_len=128000. Also compared to the fixture's copy, so
    /// the three languages cannot drift apart without a red test.
    private let goldenHeaderHex =
        "53564c54" + "01" + "01" + "57" + "00"
        + "0102030405060708"
        + "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
        + "c0c1c2c3c4c5c6c7c8c9cacb"
        + "00f40100"

    private func bytes(_ range: ClosedRange<UInt8>) -> Data { Data(Array(range)) }

    private func hex(_ s: String?, file: StaticString = #filePath, line: UInt = #line) throws -> Data {
        try XCTUnwrap(s.flatMap(Data.init(hexString:)), "not hex", file: file, line: line)
    }

    // MARK: - fixture plumbing

    /// #filePath → ios/Tests/SecuraCVTests/… → repo root is four up. Skips
    /// (rather than fails) when the checkout isn't visible from the test
    /// host; tools/test_unseal_snapshot.py remains the gate on the fixture.
    private func parityFixture() throws -> [String: Any] {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let url = repoRoot.appendingPathComponent("tools/fixtures/vault/svlt_parity.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "repo checkout not visible from the test host; "
                          + "tools/test_unseal_snapshot.py remains the gate on the fixture")
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: url))
        return try XCTUnwrap(json as? [String: Any])
    }

    // MARK: - the golden header (third language)

    func testGoldenHeaderBuildsByteForByte() throws {
        let header = try SvltHeader(trigger: .smoke, bucket: 87,
                                    keyID: bytes(0x01...0x08),
                                    ephemeralPub: bytes(0xA0...0xBF),
                                    nonce: bytes(0xC0...0xCB),
                                    ctLen: 128_000)
        XCTAssertEqual(header.bytes.count, SvltHeader.size)
        XCTAssertEqual(ChainVerifier.hex(header.bytes), goldenHeaderHex,
                       "the header layout drifted from the C++ and python golden bytes")
    }

    func testGoldenHeaderParses() throws {
        let h = try SvltHeader.parse(try hex(goldenHeaderHex))
        XCTAssertEqual(h.trigger, .smoke)
        XCTAssertEqual(h.bucket, 87)
        XCTAssertEqual(h.keyID, bytes(0x01...0x08))
        XCTAssertEqual(h.ephemeralPub, bytes(0xA0...0xBF))
        XCTAssertEqual(h.nonce, bytes(0xC0...0xCB))
        XCTAssertEqual(h.ctLen, 128_000, "ct_len is little-endian")
        XCTAssertEqual(h.keyIDHex, "0102030405060708")
    }

    func testFixtureCarriesTheSameGoldenHeader() throws {
        let fx = try parityFixture()
        XCTAssertEqual(fx["golden_header_hex"] as? String, goldenHeaderHex,
                       "the fixture's golden header is not this suite's — one side drifted")
    }

    // MARK: - header bounds (exactly unseal_snapshot.py's parse_header)

    private func mutated(_ edit: (inout [UInt8]) -> Void) throws -> Data {
        var raw = [UInt8](try hex(goldenHeaderHex))
        edit(&raw)
        return Data(raw)
    }

    func testMalformedHeadersAreRefused() throws {
        let golden = try hex(goldenHeaderHex)
        XCTAssertThrowsError(try SvltHeader.parse(golden.prefix(63))) {
            XCTAssertEqual($0 as? SnapshotVaultError, .tooShort)
        }
        XCTAssertThrowsError(try SvltHeader.parse(try mutated { $0[0] = UInt8(ascii: "X") })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .badMagic)
        }
        XCTAssertThrowsError(try SvltHeader.parse(try mutated { $0[4] = 2 })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .unsupportedVersion(2))
        }
        // 4/5 are motion/mesh; 6 is unused.
        XCTAssertThrowsError(try SvltHeader.parse(try mutated { $0[5] = 6 })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .unknownTrigger(6))
        }
        XCTAssertThrowsError(try SvltHeader.parse(try mutated { $0[6] = 144 })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .bucketOutOfRange(144))
        }
        XCTAssertThrowsError(try SvltHeader.parse(try mutated {
            $0[60] = 0; $0[61] = 0; $0[62] = 0; $0[63] = 0
        })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .ciphertextLengthOutOfRange(0))
        }
        // 512 KiB + 1, little-endian.
        let over = UInt32(SvltHeader.maxCiphertext + 1)
        XCTAssertThrowsError(try SvltHeader.parse(try mutated {
            $0[60] = UInt8(over & 0xFF); $0[61] = UInt8((over >> 8) & 0xFF)
            $0[62] = UInt8((over >> 16) & 0xFF); $0[63] = UInt8((over >> 24) & 0xFF)
        })) {
            XCTAssertEqual($0 as? SnapshotVaultError, .ciphertextLengthOutOfRange(SvltHeader.maxCiphertext + 1))
        }
        // The test trigger (9) is a real trigger.
        XCTAssertEqual(try SvltHeader.parse(try mutated { $0[5] = 9 }).trigger, .test)
    }

    func testBucketRangeIsCoarseAndNeverInvented() {
        XCTAssertEqual(SvltHeader.bucketRange(87), "14:30 – 14:40-ish")
        XCTAssertEqual(SvltHeader.bucketRange(0), "00:00 – 00:10-ish")
        XCTAssertEqual(SvltHeader.bucketRange(143), "23:50 – 00:00-ish")
        // The firmware's list answers 255 for a header it could not read.
        XCTAssertNil(SvltHeader.bucketRange(255), "an unreadable bucket must not become a time")
        XCTAssertNil(SvltHeader.bucketRange(-1))
        XCTAssertEqual(VaultItem(name: "seal_00000001_smoke.svlt", timeBucket: 255).bucketLabel,
                       "time unknown")
        XCTAssertEqual(VaultItem(name: "seal_00000001_smoke.svlt", timeBucket: nil).bucketLabel,
                       "time unknown")
    }

    // MARK: - the parity fixture: open it, then re-seal it identically

    func testParityFixtureOpensToThePinnedPlaintext() throws {
        let fx = try parityFixture()
        let key = try Curve25519.KeyAgreement.PrivateKey(
            rawRepresentation: try hex(fx["operator_priv_hex"] as? String))
        XCTAssertEqual(ChainVerifier.hex(key.publicKey.rawRepresentation),
                       fx["operator_pub_hex"] as? String, "X25519 public key derivation")
        XCTAssertEqual(ChainVerifier.hex(SnapshotSealer.keyID(of: key.publicKey)),
                       fx["key_id_hex"] as? String, "key id = SHA-256(pub)[0..<8]")

        let file = try hex(fx["file_hex"] as? String)
        let frame = try SnapshotSealer.unseal(file: file, key: key)
        XCTAssertEqual(ChainVerifier.hex(Data(SHA256.hash(data: frame.jpeg))),
                       fx["plaintext_sha256"] as? String,
                       "CryptoKit opened the python reference's file to different bytes")
        XCTAssertEqual(frame.jpeg.count, fx["plaintext_len"] as? Int)
        XCTAssertEqual(Int(frame.header.trigger.rawValue), fx["trigger"] as? Int)
        XCTAssertEqual(frame.header.trigger.tag, fx["trigger_tag"] as? String)
        XCTAssertEqual(frame.header.bucket, fx["bucket"] as? Int)
        XCTAssertEqual(ChainVerifier.hex(frame.header.bytes), fx["header_hex"] as? String)
    }

    func testParityFixtureReSealsByteForByte() throws {
        let fx = try parityFixture()
        let key = try Curve25519.KeyAgreement.PrivateKey(
            rawRepresentation: try hex(fx["operator_priv_hex"] as? String))
        let ephemeral = try Curve25519.KeyAgreement.PrivateKey(
            rawRepresentation: try hex(fx["ephemeral_priv_hex"] as? String))
        XCTAssertEqual(ChainVerifier.hex(ephemeral.publicKey.rawRepresentation),
                       fx["ephemeral_pub_hex"] as? String)
        let trigger = try XCTUnwrap(SvltTrigger(rawValue: UInt8(try XCTUnwrap(fx["trigger"] as? Int))))
        let sealed = try SnapshotSealer.seal(plain: try hex(fx["plaintext_hex"] as? String),
                                             recipient: key.publicKey,
                                             trigger: trigger,
                                             bucket: try XCTUnwrap(fx["bucket"] as? Int),
                                             ephemeral: ephemeral,
                                             nonce: try hex(fx["nonce_hex"] as? String))
        XCTAssertEqual(ChainVerifier.hex(sealed), fx["file_hex"] as? String,
                       "the Swift seal is not byte-identical to the python reference's")
    }

    // MARK: - round trip + negatives (the same set as the python tool)

    private let plain = Data((0..<4096).map { UInt8($0 & 0xFF) })

    private func sealedFixture() throws -> (file: Data, key: Curve25519.KeyAgreement.PrivateKey) {
        let key = Curve25519.KeyAgreement.PrivateKey()
        let file = try SnapshotSealer.seal(plain: plain, recipient: key.publicKey,
                                           trigger: .glass, bucket: 12)
        return (file, key)
    }

    func testSealThenUnsealRoundTrips() throws {
        let (file, key) = try sealedFixture()
        XCTAssertEqual(file.count, SvltHeader.size + plain.count + SvltHeader.tagSize)
        let frame = try SnapshotSealer.unseal(file: file, key: key)
        XCTAssertEqual(frame.jpeg, plain)
        XCTAssertEqual(frame.header.trigger, .glass)
        XCTAssertEqual(frame.header.bucket, 12)
    }

    func testWrongKeyNamesBothKeyIDs() throws {
        let (file, key) = try sealedFixture()
        let other = Curve25519.KeyAgreement.PrivateKey()
        XCTAssertThrowsError(try SnapshotSealer.unseal(file: file, key: other)) {
            XCTAssertEqual($0 as? SnapshotVaultError, .keyIDMismatch(
                file: ChainVerifier.hex(SnapshotSealer.keyID(of: key.publicKey)),
                key: ChainVerifier.hex(SnapshotSealer.keyID(of: other.publicKey))))
        }
    }

    private func flipped(_ file: Data, at index: Int) -> Data {
        var raw = [UInt8](file)
        raw[index] ^= 0x01
        return Data(raw)
    }

    func testTamperingFailsAtTheTag() throws {
        let (file, key) = try sealedFixture()
        let tagStart = file.count - SvltHeader.tagSize
        for (what, index) in [("ciphertext", SvltHeader.size + 10),
                              ("tag", tagStart + 3),
                              // Header bytes that still PARSE: the reserved
                              // byte and the nonce are authenticated as AAD.
                              ("header reserved byte (AAD)", 7),
                              ("header nonce (AAD)", 50),
                              ("header bucket (AAD)", 6)] {
            XCTAssertThrowsError(try SnapshotSealer.unseal(file: flipped(file, at: index), key: key),
                                 what) {
                XCTAssertEqual($0 as? SnapshotVaultError, .authenticationFailed, what)
            }
        }
    }

    func testTruncatedOrPaddedFilesAreRefused() throws {
        let (file, key) = try sealedFixture()
        XCTAssertThrowsError(try SnapshotSealer.unseal(file: file.dropLast(), key: key)) {
            XCTAssertEqual($0 as? SnapshotVaultError,
                           .lengthMismatch(expected: file.count, actual: file.count - 1))
        }
        XCTAssertThrowsError(try SnapshotSealer.unseal(file: file + Data([0]), key: key)) {
            XCTAssertEqual($0 as? SnapshotVaultError,
                           .lengthMismatch(expected: file.count, actual: file.count + 1))
        }
    }

    func testSealRefusesWhatTheFirmwareWouldNeverSeal() {
        let key = Curve25519.KeyAgreement.PrivateKey()
        XCTAssertThrowsError(try SnapshotSealer.seal(plain: Data(), recipient: key.publicKey,
                                                     trigger: .test, bucket: 0))
        XCTAssertThrowsError(try SnapshotSealer.seal(plain: Data(count: SvltHeader.maxCiphertext + 1),
                                                     recipient: key.publicKey,
                                                     trigger: .test, bucket: 0))
        XCTAssertThrowsError(try SnapshotSealer.seal(plain: plain, recipient: key.publicKey,
                                                     trigger: .test, bucket: 144))
        XCTAssertThrowsError(try SnapshotSealer.seal(plain: plain, recipient: key.publicKey,
                                                     trigger: .test, bucket: 0,
                                                     nonce: Data(count: 11)),
                             "a wrong-size nonce is an error, never a trap")
    }

    // MARK: - the ring filename grammar (vault_logic::filename_parse)

    func testFilenameGrammarAcceptsEveryTriggerTag() {
        for trigger in SvltTrigger.allCases {
            let name = "seal_00000042_\(trigger.tag).svlt"
            let parsed = VaultFilename.parse(name)
            XCTAssertEqual(parsed?.seq, 42, name)
            XCTAssertEqual(parsed?.trigger, trigger, name)
        }
        XCTAssertEqual(SvltTrigger(tag: "co"), .co)
        XCTAssertNil(SvltTrigger(tag: "none"))
    }

    func testFilenameGrammarRefusesEverythingElse() {
        for bad in ["", "seal_", "seal_00000001_smoke", "seal_0000001_smoke.svlt",
                    "seal_000000001_smoke.svlt", "seal_0000000a_smoke.svlt",
                    "seal_00000001-smoke.svlt", "seal_00000001_none.svlt",
                    "seal_00000001_SMOKE.svlt", "seal_00000001_smoke.SVLT",
                    "seal_00000001_smoke.svlt.jpg", "seal_00000001_smoke.svltx",
                    "../seal_00000001_smoke.svlt", "seal_00000001_../smoke.svlt",
                    "seal_00000001_smoke.svlt/..", "/VAULT/seal_00000001_smoke.svlt",
                    "seal_00000001_smo.ke.svlt", "seal_００００００01_smoke.svlt"] {
            XCTAssertFalse(VaultFilename.isValid(bad), "\"\(bad)\" must not pass the gate")
        }
    }

    // MARK: - the document door (Info.plist ↔ SvltFile)

    // @MainActor: UnsealView is a View, and on current SDKs a View's
    // static members are main-actor isolated.
    @MainActor
    func testInfoPlistExportsAndClaimsTheSvltType() throws {
        let plistURL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .appendingPathComponent("Support/Info.plist")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: plistURL.path),
                          "repo checkout not visible from the test host")
        let plist = try XCTUnwrap(
            PropertyListSerialization.propertyList(
                from: Data(contentsOf: plistURL), format: nil) as? [String: Any])

        let exported = try XCTUnwrap(plist["UTExportedTypeDeclarations"] as? [[String: Any]])
        let svlt = try XCTUnwrap(exported.first { ($0["UTTypeIdentifier"] as? String) == SvltFile.typeIdentifier },
                                 "Info.plist no longer exports \(SvltFile.typeIdentifier)")
        let tags = svlt["UTTypeTagSpecification"] as? [String: Any]
        XCTAssertEqual(tags?["public.filename-extension"] as? [String], [SvltFile.fileExtension])
        XCTAssertEqual(svlt["UTTypeConformsTo"] as? [String], ["public.data"])

        let docs = try XCTUnwrap(plist["CFBundleDocumentTypes"] as? [[String: Any]])
        let claimed = docs.flatMap { ($0["LSItemContentTypes"] as? [String]) ?? [] }
        XCTAssertTrue(claimed.contains(SvltFile.typeIdentifier),
                      "a .svlt tapped in Files would no longer open this app")
        XCTAssertEqual(UnsealView.contentType.identifier, SvltFile.typeIdentifier)
    }

    // MARK: - the Inbox rule (delete only our own copy)

    func testOnlyThisAppsInboxCopyIsEverDeleted() {
        let docs = URL(fileURLWithPath: "/private/var/mobile/Containers/Data/Application/X/Documents")
        XCTAssertTrue(UnsealModel.isOurInboxCopy(
            docs.appendingPathComponent("Inbox/seal_00000001_smoke.svlt"), documents: docs))
        // A user's own folder that happens to be called Inbox, opened in place.
        XCTAssertFalse(UnsealModel.isOurInboxCopy(
            URL(fileURLWithPath: "/private/var/mobile/Library/Mobile Documents/Inbox/seal_00000001_smoke.svlt"),
            documents: docs))
        XCTAssertFalse(UnsealModel.isOurInboxCopy(
            docs.appendingPathComponent("Inboxes/seal_00000001_smoke.svlt"), documents: docs))
        XCTAssertFalse(UnsealModel.isOurInboxCopy(
            docs.appendingPathComponent("Inbox/../seal_00000001_smoke.svlt"), documents: docs))
    }
}
