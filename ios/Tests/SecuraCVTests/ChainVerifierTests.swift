// ChainVerifierTests.swift
//
// The trust core is pure and deterministic, so it's the most valuable thing to
// pin with tests — a broken link or a bad signature must ALWAYS be caught. Runs
// on the simulator in the gated CI (see .github/workflows/ios-*.yml).

import XCTest
import CryptoKit
@testable import SecuraCV

final class ChainVerifierTests: XCTestCase {

    /// Build a valid, self-consistent chain of `n` records signed by `key`.
    private func makeChain(_ n: Int, key: Curve25519.Signing.PrivateKey) -> WitnessChainPage {
        var records: [WitnessRecord] = []
        var prev = String(repeating: "0", count: 64)
        for seq in 1...n {
            let ts = Date(timeIntervalSince1970: 1_700_000_000 + Double(seq))
            let iso = ISO8601DateFormatter.witness.string(from: ts)
            // Seven fields, exactly as device-state.js hashes them.
            let preimage = "\(seq):\(prev):\(iso):person_detected:front:device_clock:"
            let hash = ChainVerifier.sha256Hex(preimage)
            let sig = try! key.signature(for: Data(hash.utf8))
            records.append(.init(seq: UInt64(seq), hash: hash, prevHash: prev,
                                 timestamp: ts, eventType: "person_detected",
                                 zone: "front", signature: sig.map { String(format: "%02x", $0) }.joined()))
            prev = hash
        }
        return WitnessChainPage(records: records)
    }

    func testVerifiedWhenPinnedKeyMatches() {
        let key = Curve25519.Signing.PrivateKey()
        let page = makeChain(5, key: key)
        let verdict = ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation)
        XCTAssertEqual(verdict, .verified)
    }

    func testSignedUnpinnedOnFirstSight() {
        let key = Curve25519.Signing.PrivateKey()
        let page = makeChain(3, key: key)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: nil), .signedUnpinned)
    }

    func testSignatureFailsAgainstWrongKey() {
        let page = makeChain(3, key: .init())
        let attacker = Curve25519.Signing.PrivateKey().publicKey.rawRepresentation
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: attacker), .signatureFailed)
    }

    func testBrokenLinkIsDetected() {
        let key = Curve25519.Signing.PrivateKey()
        var page = makeChain(4, key: key)
        // Tamper: corrupt record #3's prev_hash so the chain no longer links.
        page.records[2].prevHash = String(repeating: "f", count: 64)
        let verdict = ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation)
        XCTAssertEqual(verdict, .brokenLink(seq: 3))
    }

    func testTamperedHashIsDetected() {
        let key = Curve25519.Signing.PrivateKey()
        var page = makeChain(4, key: key)
        page.records[1].hash = String(repeating: "a", count: 64)   // rewrite history
        if case .brokenLink = ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation) {
            // expected
        } else {
            XCTFail("a rewritten hash must be caught as a broken link")
        }
    }

    // MARK: - unsigned is never verified (spec/witness_api_v1.md §2.1)

    func testAnUnsignedHeadIsUnsignedWhateverKeyWeHold() {
        let key = Curve25519.Signing.PrivateKey()
        var page = makeChain(3, key: key)
        page.records[2].signature = ""
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation), .unsigned)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: nil), .unsigned)
        XCTAssertEqual(ChainVerdict.unsigned.badge, .unsigned)
        XCTAssertFalse(ChainVerdict.unsigned.badge.isTrusted)
    }

    // MARK: - the wap_v1 construction, independent of the shared fixture

    /// A canary-wap chain built exactly as the firmware builds one
    /// (compute_chain_hash + Ed25519 over the raw hash), from the genesis
    /// the device would use. The fixture suite pins the real bytes; this
    /// pins the verifier's arithmetic against a fresh key every run.
    private func makeWapChain(_ n: Int, key: Curve25519.Signing.PrivateKey,
                              deviceID: String = "canary-test-0001") -> WitnessChainPage {
        var records: [WitnessRecord] = []
        var prev = ChainVerifier.wapGenesis(deviceID: deviceID)
        for seq in 1...n {
            let payloadHash = Data(SHA256.hash(data: Data("payload-\(seq)".utf8)))
            let bucket = UInt32(100 + seq)
            let hash = ChainVerifier.wapChainHash(prevHash: prev, payloadHash: payloadHash,
                                                  seq: UInt32(seq), timeBucket: bucket)
            let sig = try! key.signature(for: hash)              // the RAW 32 bytes
            records.append(.init(seq: UInt64(seq), hash: ChainVerifier.hex(hash),
                                 prevHash: ChainVerifier.hex(prev),
                                 timestamp: Date(timeIntervalSince1970: 1_788_864_000),
                                 eventType: "witness_event", zone: "",
                                 signature: ChainVerifier.hex(sig),
                                 payloadHash: ChainVerifier.hex(payloadHash),
                                 timeBucket: bucket, timeBucketMS: 5000, recordType: 1))
            prev = hash
        }
        return WitnessChainPage(records: records, chainFormat: "wap_v1",
                                deviceID: deviceID, total: UInt64(n), uptimeS: 700)
    }

    func testWapChainVerifiesAgainstItsKey() {
        let key = Curve25519.Signing.PrivateKey()
        let page = makeWapChain(5, key: key)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation), .verified)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: nil), .signedUnpinned)
    }

    func testWapSignatureIsOverTheRawHashNotTheHexString() {
        // The reference server signs the hex string's UTF-8; canary-wap
        // signs the raw 32 bytes. A verifier that used the wrong message
        // would fail every healthy WAP — so pin which one it is.
        let key = Curve25519.Signing.PrivateKey()
        var page = makeWapChain(2, key: key)
        let head = page.records[1]
        let overHex = try! key.signature(for: Data(head.hash.utf8))
        page.records[1].signature = ChainVerifier.hex(overHex)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation),
                       .signatureFailed)
    }

    func testWapBrokenLinkAndEditedBucketAreCaught() {
        let key = Curve25519.Signing.PrivateKey()
        var page = makeWapChain(4, key: key)
        page.records[2].prevHash = String(repeating: "f", count: 64)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation),
                       .brokenLink(seq: 3))

        var edited = makeWapChain(4, key: key)
        edited.records[1].timeBucket = 999      // the bucket is in the hash — edits show
        XCTAssertEqual(ChainVerifier.verify(edited, pinnedKey: key.publicKey.rawRepresentation),
                       .brokenLink(seq: 2))
    }

    func testWapChainDoesNotDependOnTheWallClock() {
        // The wap hash never covered timestamp — a clockless page (anchored
        // locally) verifies exactly like a dated one.
        let key = Curve25519.Signing.PrivateKey()
        var page = makeWapChain(3, key: key)
        for i in page.records.indices {
            page.records[i].hasWireTimestamp = false
            page.records[i].timestamp = .distantPast
        }
        page.anchorMissingTimestamps(fetchedAt: Date())
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation), .verified)
    }

    func testReferenceChainStillVerifiesWithoutAChainFormat() {
        // The reference device-api sends no chain_format; absent means
        // reference_v1 and everything above this test keeps working.
        let key = Curve25519.Signing.PrivateKey()
        let page = makeChain(3, key: key)
        XCTAssertNil(page.chainFormat)
        XCTAssertEqual(page.format, .referenceV1)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: key.publicKey.rawRepresentation), .verified)
    }
}
