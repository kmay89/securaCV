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
        XCTAssertNil(row.suffixAmbiguous)
    }

    func testThePhonesTwinVerdictRoundTrips() throws {
        // The verdict is computed phone-side against the FULL fleet, because
        // the snapshot's rows are capped and a wrist-side check could miss a
        // twin the cap dropped.
        var snapshot = WristSnapshot.sample()
        snapshot.witnesses[0].fingerprint = "aaaaaaaaaaaa0708"
        snapshot.witnesses[0].suffixAmbiguous = true
        let context = try WristSync.context(for: snapshot)
        let decoded = try XCTUnwrap(WristSync.snapshot(fromContext: context))
        XCTAssertEqual(decoded.witnesses[0].suffixAmbiguous, true)
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

    // MARK: - the complication's one-tap target

    private func freshDefaults() throws -> (UserDefaults, String) {
        let suite = "test-last-find-\(UUID().uuidString)"
        return (try XCTUnwrap(UserDefaults(suiteName: suite)), suite)
    }

    func testLastFindRoundTripsThroughInjectedDefaults() throws {
        let (defaults, suite) = try freshDefaults()
        defer { defaults.removePersistentDomain(forName: suite) }
        XCTAssertNil(WristLastFind.load(from: defaults))
        WristLastFind.remember("canary-porch-01", in: defaults)
        XCTAssertEqual(WristLastFind.load(from: defaults), "canary-porch-01")
    }

    func testAFindableTargetNeedsARowAndARecognizableBeacon() {
        var snapshot = WristSnapshot.sample()
        snapshot.witnesses[0].fingerprint = "aaaaaaaaaaaa0708"
        let id = snapshot.witnesses[0].id
        // Row present, beacon recognizable → the one honest deep link.
        XCTAssertEqual(WristLastFind.findableTarget(id: id, in: snapshot)?.id, id)
        // Nothing remembered, or nothing cached → no link, never a dead end.
        XCTAssertNil(WristLastFind.findableTarget(id: nil, in: snapshot))
        XCTAssertNil(WristLastFind.findableTarget(id: id, in: nil))
        // A remembered id the fleet no longer holds → no link.
        XCTAssertNil(WristLastFind.findableTarget(id: "gone", in: snapshot))
        // Row present but no fingerprint (an older phone) → no link: the
        // search it promises couldn't recognize the beacon.
        snapshot.witnesses[0].fingerprint = nil
        XCTAssertNil(WristLastFind.findableTarget(id: id, in: snapshot))
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
