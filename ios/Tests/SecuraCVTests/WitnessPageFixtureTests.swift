// WitnessPageFixtureTests.swift
//
// The other end of ONE fixture. spec/fixtures/witness_page_v1.json is the
// byte-exact `GET /api/v1/witness` page a canary-wap renders for four
// records (chain_format wap_v1); the firmware host test
// (firmware/projects/canary-wap/tests_host/test_witness_page.cpp) rebuilds
// it from real Ed25519-signed records and byte-compares. This suite decodes
// the SAME file with the app's decoder and expects ChainVerifier to reach
// `.verified` against the SAME public key — so the contract cannot drift on
// either side without one of the two going red. It exists because the two
// ends had drifted completely: the app fetched a page no firmware in this
// repository served (docs/IMPROVEMENT_ROADMAP.md row 6).
//
// Compile status: written without an Apple toolchain in reach; the gated
// iOS CI is the compiler. The fixture path follows the repo-relative
// #filePath idiom the other fixture-reading suites use.

import XCTest
import CryptoKit
@testable import SecuraCV

final class WitnessPageFixtureTests: XCTestCase {

    /// The fixture's TEST-ONLY signing key's public half — the same constant
    /// test_witness_page.cpp pins (PUBKEY_HEX), printed by the generator.
    private static let pubkeyHex = "22b7279917b3a47068f4d17f0ee3182b5fef9035592765f9b55e4eb97d167e1c"

    private var pinnedKey: Data { Data(hexString: Self.pubkeyHex)! }

    /// #filePath → ios/Tests/SecuraCVTests/… → repo root is four up.
    private func fixtureText() throws -> String {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let url = repoRoot.appendingPathComponent("spec/fixtures/witness_page_v1.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "repo checkout not visible from the test host; " +
                          "the firmware host test remains the primary gate on the fixture")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// Decode exactly as DeviceAPI does — same decoder, same date strategy.
    private func decode(_ text: String) throws -> WitnessChainPage {
        try DeviceAPI.decoder.decode(WitnessChainPage.self, from: Data(text.utf8))
    }

    // MARK: - shape

    func testFixtureDecodesInTheContractShape() throws {
        let page = try decode(try fixtureText())
        XCTAssertEqual(page.schema, "securacv/witness_page/v1")
        XCTAssertEqual(page.chainFormat, "wap_v1")
        XCTAssertEqual(page.format, .wapV1)
        XCTAssertEqual(page.deviceID, "canary-fixture-0001")
        XCTAssertEqual(page.total, 4)
        XCTAssertEqual(page.uptimeS, 700)
        XCTAssertEqual(page.records.map(\.seq), [1, 2, 3, 4], "oldest first")
        XCTAssertEqual(page.records.map(\.eventType),
                       ["boot_attestation", "witness_event", "tamper_detected", "state_change"])
        XCTAssertEqual(page.records.map(\.recordType), [0, 1, 2, 3])
        for r in page.records {
            XCTAssertEqual(r.hash.count, 64)
            XCTAssertEqual(r.prevHash.count, 64)
            XCTAssertEqual(r.payloadHash?.count, 64)
            XCTAssertEqual(r.signature.count, 128, "every wap record is individually signed")
            XCTAssertEqual(r.timeBucketMS, 5000)
            XCTAssertEqual(r.zone, "")
            XCTAssertEqual(r.timeSource, "device_clock")
            XCTAssertTrue(r.hasWireTimestamp)
        }
        XCTAssertEqual(page.records.map(\.timeBucket), [1, 130, 131, 132])
    }

    func testTimestampsAreCoarseBucketStarts() throws {
        let page = try decode(try fixtureText())
        // 2026-09-08T10:20:00Z and 10:30:00Z — the device's clock read
        // 10:40:00Z at render, the records were 695 s / ≤50 s old, and each
        // is floored to its ten-minute bucket (Invariant III).
        let t = page.records.map { $0.timestamp.timeIntervalSince1970 }
        XCTAssertEqual(t[0], 1_788_862_800)
        XCTAssertEqual(t[1], 1_788_863_400)
        XCTAssertEqual(t[2], 1_788_863_400)
        XCTAssertEqual(t[3], 1_788_863_400)
        for x in t { XCTAssertEqual(x.truncatingRemainder(dividingBy: 600), 0, "never finer than ten minutes") }
    }

    func testTamperRecordCarriesTheTamperSeverity() throws {
        let page = try decode(try fixtureText())
        XCTAssertEqual(page.records[2].severity, .tamper,
                       "the dictionary id on the wire is what makes the tamper severity fire")
        XCTAssertEqual(page.records[0].severity, .notice, "chain bookkeeping is a calm line")
    }

    // MARK: - the trust ladder against the fixture

    func testFixtureVerifiesAgainstThePinnedKey() throws {
        let page = try decode(try fixtureText())
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .verified)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey).badge, .verified)
    }

    func testFirstSightIsSignedNotVerified() throws {
        let page = try decode(try fixtureText())
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: nil), .signedUnpinned)
    }

    func testWrongKeyFailsLoudly() throws {
        let page = try decode(try fixtureText())
        let attacker = Curve25519.Signing.PrivateKey().publicKey.rawRepresentation
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: attacker), .signatureFailed)
    }

    func testEveryRecordRecomputesUnderTheWapConstruction() throws {
        let page = try decode(try fixtureText())
        for r in page.records {
            XCTAssertEqual(ChainVerifier.recomputedHash(of: r, format: .wapV1), r.hash, "seq \(r.seq)")
        }
        // And seq 1 chains from the device's genesis — the fixture starts
        // at the very first record, so the optional check is checkable here.
        XCTAssertEqual(page.records[0].prevHash,
                       ChainVerifier.hex(ChainVerifier.wapGenesis(deviceID: "canary-fixture-0001")))
    }

    // MARK: - tamper is caught

    func testEditedSeqBreaksTheChain() throws {
        // The signature covers only the hash, so an edited seq with a
        // genuine hash/signature pair must fall to the recompute, not pass.
        var page = try decode(try fixtureText())
        page.records[1].seq = 20
        // The forged record itself no longer recomputes: seq is in the
        // wap_v1 pre-image (spec §4.2), so its stored hash is now wrong.
        XCTAssertNotEqual(ChainVerifier.recomputedHash(of: page.records[1], format: .wapV1),
                          page.records[1].hash)
        // The verdict names the FIRST break in ascending seq order, and the
        // edit reordered the walk to 1, 3, 4, 20: record 3's prev_hash is
        // the genuine hash of the record that used to be seq 2, which no
        // longer precedes it — so the walk stops at 3 before it ever reaches
        // the forgery. A failed verdict is the contract; the seq is a hint to
        // where the chain first stops making sense, not an accusation.
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .brokenLink(seq: 3))
    }

    func testEditedPayloadHashBreaksTheChain() throws {
        var page = try decode(try fixtureText())
        page.records[2].payloadHash = String(repeating: "0", count: 64)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .brokenLink(seq: 3))
    }

    func testRewrittenHashBreaksTheChain() throws {
        var page = try decode(try fixtureText())
        page.records[1].hash = String(repeating: "a", count: 64)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .brokenLink(seq: 2))
    }

    func testWapRecordMissingItsPreimageCannotVerify() throws {
        var page = try decode(try fixtureText())
        page.records[3].payloadHash = nil
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .brokenLink(seq: 4))
    }

    // MARK: - unsigned is never verified

    func testEmptyHeadSignatureIsUnsigned() throws {
        var page = try decode(try fixtureText())
        page.records[3].signature = ""
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .unsigned)
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey).badge, .unsigned)
    }

    func testAbsentHeadSignatureDecodesAndIsUnsigned() throws {
        // Strip the LAST record's signature key entirely — the contract
        // allows an absent signature; the decoder must not throw, and the
        // verdict must be Unsigned even with the right key in hand.
        let text = try fixtureText()
        guard let sigRange = text.range(of: ",\"signature\":\"", options: .backwards),
              let close = text.range(of: "\"", range: sigRange.upperBound..<text.endIndex) else {
            return XCTFail("fixture has no trailing signature to strip")
        }
        var stripped = text
        stripped.removeSubrange(sigRange.lowerBound..<close.upperBound)
        let page = try decode(stripped)
        XCTAssertEqual(page.records.count, 4)
        XCTAssertEqual(page.records[3].signature, "")
        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .unsigned)
    }

    // MARK: - no clock on the device

    func testClocklessPageAnchorsCoarselyAndStillVerifies() throws {
        // A canary-wap that has not met a clock sends no timestamp and no
        // time_source at all (never a 1970 date). The decoder anchors each
        // record from time_bucket × time_bucket_ms against uptime_s and the
        // fetch time, floored to ten minutes — and the chain still verifies,
        // because the wap hash never covered the wall clock.
        let text = try fixtureText()
        let clockless = text.replacingOccurrences(
            of: #""time_source":"device_clock","timestamp":"[^"]*","#,
            with: "", options: .regularExpression)
        XCTAssertFalse(clockless.contains("timestamp"), "the strip must remove every timestamp")
        let before = Date()
        let page = try decode(clockless)
        let after = Date()

        XCTAssertEqual(page.records.count, 4)
        for r in page.records {
            XCTAssertFalse(r.hasWireTimestamp)
            XCTAssertEqual(r.timestamp.timeIntervalSince1970.truncatingRemainder(dividingBy: 600), 0,
                           "anchored time stays coarse")
            XCTAssertLessThanOrEqual(r.timestamp, after)
            XCTAssertGreaterThan(r.timestamp, before.addingTimeInterval(-1_800),
                                 "a 700 s-old record anchors within the last half hour")
        }
        // Record 1 is 645 s older than record 2 (buckets 1 vs 130 at 5 s):
        // after flooring that is one or two ten-minute steps, never zero.
        let gap = page.records[1].timestamp.timeIntervalSince(page.records[0].timestamp)
        XCTAssertTrue(gap == 600 || gap == 1_200, "gap was \(gap)")
        // Records 2–4 are within 10 s of each other: same bucket or adjacent.
        let gap23 = page.records[2].timestamp.timeIntervalSince(page.records[1].timestamp)
        XCTAssertTrue(gap23 == 0 || gap23 == 600)

        XCTAssertEqual(ChainVerifier.verify(page, pinnedKey: pinnedKey), .verified)
    }

    func testAnchoringIsDeterministicForAGivenFetchTime() throws {
        var page = try decode(try fixtureText())
        for i in page.records.indices { page.records[i].hasWireTimestamp = false }
        // Fetched exactly at 2026-09-08T10:40:00Z: the anchors must equal
        // the timestamps the device itself wrote for that same clock.
        page.anchorMissingTimestamps(fetchedAt: Date(timeIntervalSince1970: 1_788_864_000))
        XCTAssertEqual(page.records.map { $0.timestamp.timeIntervalSince1970 },
                       [1_788_862_800, 1_788_863_400, 1_788_863_400, 1_788_863_400])
    }

    // MARK: - an unknown format is unverified, not a guess

    func testUnknownChainFormatIsUnverifiedNotTamper() throws {
        var page = try decode(try fixtureText())
        page.chainFormat = "wap_v9"
        let verdict = ChainVerifier.verify(page, pinnedKey: pinnedKey)
        XCTAssertEqual(verdict, .unsupportedFormat("wap_v9"))
        XCTAssertEqual(verdict.badge, .unknown)
    }

    func testAbsentChainFormatMeansTheReferenceServer() {
        XCTAssertEqual(WitnessChainFormat.from(wire: nil), .referenceV1)
        XCTAssertEqual(WitnessChainFormat.from(wire: ""), .referenceV1)
        XCTAssertEqual(WitnessChainFormat.from(wire: "wap_v1"), .wapV1)
        XCTAssertNil(WitnessChainFormat.from(wire: "something-else"))
    }
}
