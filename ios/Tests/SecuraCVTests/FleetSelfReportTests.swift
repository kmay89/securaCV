// FleetSelfReportTests.swift
//
// Pins the `/api/fleet` decoder against the REAL bytes the firmware emits.
//
// The two JSON literals below are not hand-written: they are the verbatim
// stdout of `fleet_selfreport_build()` from
// `firmware/common/fleet_selfreport/fleet_selfreport.h`, compiled with g++ and
// run. So this suite fails if the app's decoder and the firmware's builder ever
// disagree — the same parity discipline as FleetBeaconTests.

import XCTest
@testable import SecuraCV

final class FleetSelfReportTests: XCTestCase {

    /// A WAP-class device with a verifying chain — and a name containing quotes,
    /// to prove the firmware's JSON escaping survives a real decode.
    static let wapBody = #"""
    {"kernel":"Front \"Door\"","verified_through":"now","devices":[{"name":"Front \"Door\"","online":true,"chain":"ok","product":"canary-wap","chain_height":42}]}
    """#

    /// A display: online, but holds no witness chain of its own, so `chain` is
    /// "unknown" and `chain_height` is omitted entirely.
    static let displayBody = #"""
    {"kernel":"Nightstand","verified_through":"now","devices":[{"name":"Nightstand","online":true,"chain":"unknown","product":"canary-display"}]}
    """#

    func testDecodesTheFirmwaresOwnOutput() throws {
        let r = try FleetSelfReport.decode(Data(Self.wapBody.utf8))
        XCTAssertEqual(r.kernel, #"Front "Door""#, "escaped quotes survive the round trip")
        XCTAssertEqual(r.verifiedThrough, "now")
        XCTAssertEqual(r.devices.count, 1)

        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.name, #"Front "Door""#)
        XCTAssertTrue(d.online)
        XCTAssertEqual(d.product, "canary-wap")
        XCTAssertEqual(d.deviceType, .wap, "the product string maps to a device type")
        XCTAssertEqual(d.chainHeight, 42)
        XCTAssertTrue(d.chainVerifies)
    }

    func testDecodesADisplayWithNoChain() throws {
        let r = try FleetSelfReport.decode(Data(Self.displayBody.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.deviceType, .display)
        XCTAssertNil(d.chainHeight, "an omitted chain_height stays nil, never 0")
        XCTAssertFalse(d.chainVerifies, #""unknown" is not a verifying chain"#)
    }

    /// Only the exact string "ok" counts. A newer firmware inventing a value —
    /// or an attacker choosing one — must not read as a good chain.
    func testOnlyLiteralOkCountsAsVerifying() {
        for word in ["unknown", "OK", "ok ", "degraded", "true", "", "okay"] {
            let d = FleetSelfDevice(name: "x", online: true, chain: word, product: "canary-wap")
            XCTAssertFalse(d.chainVerifies, "\(word.debugDescription) must not read as a verifying chain")
        }
        XCTAssertTrue(FleetSelfDevice(name: "x", online: true, chain: "ok", product: "canary-wap").chainVerifies)
    }

    /// A Canary that answers at all is worth showing, so a partial body must
    /// degrade field-by-field instead of failing the whole decode.
    func testTolerantOfMissingAndUnknownFields() throws {
        let partial = #"{"kernel":"K","devices":[{"name":"Half","future_key":123}]}"#
        let r = try FleetSelfReport.decode(Data(partial.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.name, "Half")
        XCTAssertFalse(d.online, "a missing online field means we were not told it is up")
        XCTAssertEqual(d.deviceType, .unknown, "an absent product is an unknown type, not a crash")
        XCTAssertEqual(r.verifiedThrough, "", "a missing freshness word decodes empty")
    }

    /// A hub answers for itself and its peers — never assume one row.
    func testHubWithSeveralDevices() throws {
        let hub = #"""
        {"kernel":"Hub","verified_through":"now","devices":[\#
        {"name":"Hub","online":true,"chain":"unknown","product":"canary-display"},\#
        {"name":"Front Door","online":true,"chain":"ok","product":"canary-wap","chain_height":7},\#
        {"name":"Nursery","online":false,"chain":"unknown","product":"canary-sense"}]}
        """#
        let r = try FleetSelfReport.decode(Data(hub.utf8))
        XCTAssertEqual(r.devices.count, 3)
        XCTAssertEqual(r.devices.map(\.deviceType), [.display, .wap, .sense])
        XCTAssertFalse(r.devices[2].online)
    }

    func testGarbageIsAnError() {
        XCTAssertThrowsError(try FleetSelfReport.decode(Data("not json".utf8)))
    }
}
