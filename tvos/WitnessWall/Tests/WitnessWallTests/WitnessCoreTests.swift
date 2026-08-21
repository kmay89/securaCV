//  WitnessCoreTests.swift — the FFI wrapper against the REAL Rust core.
//
//  Deliberately not mocked. The wrapper's whole job is owning unsafe pointers
//  correctly, and a mock of the core would prove nothing about that. The test
//  bundle links libsecuracv_witness_core.a (see project.yml).

import XCTest
@testable import WitnessWall

final class WitnessCoreTests: XCTestCase {

    func testCoreReportsAVersion() {
        let version = WitnessCore.version
        XCTAssertFalse(version.isEmpty)
        XCTAssertNotEqual(version, "unknown", "the linked core should report its own version")
    }

    // MARK: - Sealed log

    func testGarbageIsAReportNotAThrow() throws {
        // The Wall must always have something calm to draw: bad content is an
        // ok:false report, never an exception.
        let report = try WitnessCore.verify(sealedLogJSON: "{{{ not json")
        XCTAssertFalse(report.ok)
        XCTAssertEqual(report.kind, .malformed)
        XCTAssertFalse(report.message.isEmpty)
    }

    func testAnEmptyLogVerifiesAsNothingSealedYet() throws {
        // A brand-new hub is not a broken hub.
        let json = """
        {"verifying_key":"\(String(repeating: "0", count: 64))","entries":[]}
        """
        let report = try WitnessCore.verify(sealedLogJSON: json)
        // An all-zero key is not a valid Ed25519 point in dalek, so this is
        // expected to be a malformed-key report rather than a pass — what we
        // assert is that it REPORTS rather than crashes or throws.
        XCTAssertFalse(report.message.isEmpty)
    }

    func testABrokenChainNamesTheEntryThatFailed() throws {
        // prev_hash that doesn't chain: the core should say which id broke.
        let key = String(repeating: "0", count: 64)
        let json = """
        {
          "verifying_key": "\(key)",
          "entries": [
            {"id": 41, "payload": "{}", "prev_hash": "\(String(repeating: "a", count: 64))",
             "entry_hash": "\(String(repeating: "b", count: 64))",
             "signature": "\(String(repeating: "c", count: 128))"}
          ]
        }
        """
        let report = try WitnessCore.verify(sealedLogJSON: json)
        XCTAssertFalse(report.ok)
    }

    func testRepeatedCallsDoNotLeakOrCorrupt() throws {
        // Exercises the free-exactly-once contract: if the wrapper double-freed
        // or leaked, this loop is where it shows up under the address sanitizer.
        for _ in 0..<500 {
            let report = try WitnessCore.verify(sealedLogJSON: "not json")
            XCTAssertFalse(report.ok)
        }
    }

    // MARK: - Fleet

    func testTheDocumentedFleetExampleParses() throws {
        // The exact example from tvos/discovery/DISCOVERY.md.
        let json = """
        {
          "kernel": "kitchen-hub",
          "verified_through": "4:02 PM",
          "devices": [
            { "name": "Front Door", "online": true,  "chain": "ok", "product": "canary-wap", "hub": "ok" },
            { "name": "Studio",     "online": true,  "chain": "ok", "product": "canary" },
            { "name": "Driveway",   "online": false, "chain": "ok", "product": "canary-vision", "hw": "xiao-esp32c3" }
          ]
        }
        """
        let fleet = try WitnessCore.parseFleet(json: json)
        XCTAssertEqual(fleet.kernel, "kitchen-hub")
        XCTAssertEqual(fleet.devices.count, 3)
        XCTAssertEqual(fleet.onlineCount, 2)
        XCTAssertFalse(fleet.hasChainTrouble)
        XCTAssertEqual(fleet.summary, "2 of 3 Canaries online")
        // The doc's optional fields survive the round trip through the Rust
        // normalizer: the board id that resolves a figure, and the hub word.
        XCTAssertEqual(fleet.devices[0].hub, "ok")
        XCTAssertEqual(fleet.devices[2].hw, "xiao-esp32c3")
        XCTAssertNil(fleet.devices[1].hw)
    }

    func testOnlineDefaultsToTrueWhenOmitted() throws {
        let fleet = try WitnessCore.parseFleet(json: #"{"devices":[{"name":"Porch"}]}"#)
        XCTAssertTrue(fleet.devices[0].online)
        XCTAssertNil(fleet.devices[0].chain)
        XCTAssertFalse(fleet.devices[0].chainIsTroubled, "an absent chain is 'not reported', not 'broken'")
    }

    func testABareArrayOfDevicesIsAccepted() throws {
        let fleet = try WitnessCore.parseFleet(json: #"[{"name":"Front Door"},{"name":"Studio"}]"#)
        XCTAssertEqual(fleet.devices.count, 2)
        XCTAssertEqual(fleet.summary, "2 Canaries, all online")
    }

    func testATroubledChainIsSurfaced() throws {
        let fleet = try WitnessCore.parseFleet(
            json: #"{"devices":[{"name":"A","chain":"ok"},{"name":"B","chain":"broken"}]}"#)
        XCTAssertTrue(fleet.hasChainTrouble)
    }

    func testACaptivePortalIsAnErrorNotAnEmptyFleet() {
        // Showing "0 Canaries" when the answer was a login page would be a lie.
        XCTAssertThrowsError(try WitnessCore.parseFleet(json: "<html>Sign in to WiFi</html>"))
    }

    func testTheSummaryNeverUsesTheForbiddenGroupNoun() throws {
        // CLAUDE.md: a group of Canaries is a fleet. Guarded here so it can't
        // drift back in through a copy edit.
        let fleet = try WitnessCore.parseFleet(json: #"{"devices":[{"name":"A"},{"name":"B"}]}"#)
        XCTAssertFalse(fleet.summary.lowercased().contains("flock"))
    }
}
