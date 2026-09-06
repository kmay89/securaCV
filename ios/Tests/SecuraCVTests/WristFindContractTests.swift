// WristFindContractTests.swift
//
// The wrist-finding slice of the phone→watch contract, held still: the new
// fields ride as ADDITIVE OPTIONALS (an old phone's payload still decodes,
// with the honest nils), consent defaults to the safe reading, the twin
// rule is one shared definition, and the identify verb's wire vocabulary
// can't drift between the two ends because both compile this file's source.

import XCTest
@testable import SecuraCV

final class WristFindContractTests: XCTestCase {
    /// A row exactly as an OLDER phone would send it — no fingerprint field
    /// at all. Decoding must succeed and the wrist must simply not offer
    /// finding, never crash or invent a match.
    func testAnOlderPhonesRowDecodesWithNoFingerprint() throws {
        let json = """
        {"id":"canary-a3f7","name":"Porch","severityRaw":0,"linkRaw":1,
         "badgeRaw":3,"tamper":false,"isMuted":false}
        """.data(using: .utf8)!
        let row = try WristSync.makeDecoder().decode(WristWitness.self, from: json)
        XCTAssertNil(row.fingerprint)
    }

    func testAnOlderPhonesSnapshotReadsAsNotConsented() throws {
        var snapshot = WristSnapshot.sample()
        snapshot.discoveryConsented = nil
        let data = try WristSync.makeEncoder().encode(snapshot)
        let decoded = try WristSync.makeDecoder().decode(WristSnapshot.self, from: data)
        // nil is the safe reading for a radio decision — the wrist must not
        // scan for a user whose phone never said yes.
        XCTAssertNil(decoded.discoveryConsented)
        XCTAssertNotEqual(decoded.discoveryConsented, true)
    }

    func testTheNewFieldsRoundTrip() throws {
        var snapshot = WristSnapshot.sample()
        snapshot.discoveryConsented = true
        snapshot.witnesses[0].fingerprint = "a1b2c3d4e5f60708"
        let context = try WristSync.context(for: snapshot)
        let decoded = try XCTUnwrap(WristSync.snapshot(fromContext: context))
        XCTAssertEqual(decoded.discoveryConsented, true)
        XCTAssertEqual(decoded.witnesses[0].fingerprint, "a1b2c3d4e5f60708")
    }

    func testAdditiveFieldsDidNotBumpTheSchema() {
        // The whole point of additive optionals: an old watch keeps reading
        // a new phone. A bump here would strand every un-updated wrist.
        XCTAssertEqual(WristSnapshot.schemaVersion, 1)
    }

    // MARK: - the twin rule, shared

    func testTwinsAreAmbiguous() {
        XCTAssertTrue(ProximityRanger.isSuffixAmbiguous(
            fingerprint: "aaaaaaaaaaaa0708",
            among: ["aaaaaaaaaaaa0708", "bbbbbbbbbbbb0708"]))
    }

    func testAUniqueSuffixIsNot() {
        XCTAssertFalse(ProximityRanger.isSuffixAmbiguous(
            fingerprint: "aaaaaaaaaaaa0708",
            among: ["aaaaaaaaaaaa0708", "bbbbbbbbbbbb0999"]))
    }

    func testAShortFingerprintNeverMatchesAnything() {
        XCTAssertFalse(ProximityRanger.isSuffixAmbiguous(
            fingerprint: "07",
            among: ["07", "aaaaaaaaaaaa0708"]))
    }

    func testTheRuleIsCaseInsensitive() {
        XCTAssertTrue(ProximityRanger.isSuffixAmbiguous(
            fingerprint: "AAAAAAAAAAAA0708",
            among: ["AAAAAAAAAAAA0708", "bbbbbbbbbbbb0708"]))
    }

    // MARK: - the identify verb's vocabulary

    func testIdentifyKeysAreStable() {
        // Both ends compile this file's source, so these can only drift if
        // someone edits the constant — and then this test names the break.
        XCTAssertEqual(WristSync.commandIdentify, "identify")
        XCTAssertEqual(WristSync.identifyIDKey, "id")
        XCTAssertEqual(WristSync.identifyOKKey, "chirpOK")
        XCTAssertEqual(WristSync.identifyVisualOnlyKey, "chirpVisualOnly")
        XCTAssertEqual(WristSync.identifyWhyKey, "chirpWhy")
    }
}
