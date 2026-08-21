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
}
