// WitnessChainPreimageTests.swift
//
// Cross-implementation pin for the witness-chain hash preimage. The vectors
// below were produced by the DEVICE's own hashing code (canary-vision
// device-api/lib/device-state.js addWitnessRecord — the seven-field template
// string fed to SHA-256), so this suite fails if either end drifts. It exists
// because the two ends DID drift once: this app hashed five fields while the
// device hashed seven (time_source, gps_timestamp), so the first live chain
// verification would have reported tamper on every healthy device.

import XCTest
@testable import SecuraCV

final class WitnessChainPreimageTests: XCTestCase {

    // Vector 1: the everyday record — device clock, no GPS fix, so the wire
    // carries no gps_timestamp field and it hashes as the empty string.
    //   data = "1:<64 zeros>:2026-08-21T09:00:00.000Z:person_detected:front:device_clock:"
    private let v1JSON = """
    {"seq":1,
     "hash":"68d316e61b1489f91349203f1753734f81947764224cec68dd738c418168f86c",
     "prev_hash":"0000000000000000000000000000000000000000000000000000000000000000",
     "timestamp":"2026-08-21T09:00:00.000Z",
     "event_type":"person_detected","zone":"front",
     "time_source":"device_clock","signature":""}
    """

    // Vector 2: a GPS-timed record — both trailing fields populated.
    private let v2JSON = """
    {"seq":2,
     "hash":"bd9a4e13c8eb2c41b4edcd4f8e273f4654947387aaffbc66da81206f1f38ab88",
     "prev_hash":"68d316e61b1489f91349203f1753734f81947764224cec68dd738c418168f86c",
     "timestamp":"2026-08-21T09:00:01.500Z",
     "event_type":"vehicle_detected","zone":"drive",
     "time_source":"gps_utc","gps_timestamp":"2026-08-21T09:00:01.000Z",
     "signature":""}
    """

    private func decode(_ json: String) throws -> WitnessRecord {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .custom { d in
            let s = try d.singleValueContainer().decode(String.self)
            guard let date = ISO8601DateFormatter.witness.date(from: s) else {
                throw DecodingError.dataCorrupted(.init(codingPath: d.codingPath,
                    debugDescription: "bad date \(s)"))
            }
            return date
        }
        return try decoder.decode(WitnessRecord.self, from: Data(json.utf8))
    }

    func testDeviceClockRecordRecomputesTheDeviceHash() throws {
        let r = try decode(v1JSON)
        XCTAssertEqual(r.timeSource, "device_clock")
        XCTAssertEqual(r.gpsTimestamp, "")
        XCTAssertEqual(ChainVerifier.sha256Hex(r.hashPreimage), r.hash,
                       "five-field preimage regression: \(r.hashPreimage)")
    }

    func testGpsRecordRecomputesTheDeviceHash() throws {
        let r = try decode(v2JSON)
        XCTAssertEqual(r.timeSource, "gps_utc")
        XCTAssertEqual(r.gpsTimestamp, "2026-08-21T09:00:01.000Z")
        XCTAssertEqual(ChainVerifier.sha256Hex(r.hashPreimage), r.hash)
    }

    func testTwoVectorRecordsChainAndVerifyAsIntact() throws {
        let page = WitnessChainPage(records: [try decode(v1JSON), try decode(v2JSON)])
        XCTAssertNil(ChainVerifier.firstBrokenLink(in: page.records),
                     "the device-produced vectors must link cleanly")
    }

    func testMissingTimeSourceDefaultsToDeviceClock() throws {
        // A hypothetical pre-time_source record: absent field must decode as
        // the device default, never crash or hash differently than the device.
        let legacy = v1JSON.replacingOccurrences(of: "\"time_source\":\"device_clock\",", with: "")
        let r = try decode(legacy)
        XCTAssertEqual(r.timeSource, "device_clock")
    }
}
