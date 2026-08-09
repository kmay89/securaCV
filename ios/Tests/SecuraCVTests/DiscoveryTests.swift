// DiscoveryTests.swift
//
// The mDNS fold: many adverts in, one row per physical device out.
//
// This is a two-sided problem and both sides have been wrong. Too FEW rows
// and a device the user owns vanishes from the app entirely — it isn't listed,
// and FleetStore never polls it, so it isn't in the fleet either. Too MANY and
// one Canary appears twice, sharing an `Identifiable` id with itself, which
// SwiftUI resolves by reusing or dropping rows.
//
// The key that gets both right is the mDNS hostname, and the reason is in the
// firmware: `make_hostname` (net/discovery.cpp) has always appended the salted
// per-unit pseudonym, so interface copies of one device share a hostname and
// two devices never do — including two devices whose `device_id` is the same
// compile-time model default.

import XCTest
@testable import SecuraCV

final class DiscoveryTests: XCTestCase {

    private func advert(service: String, deviceID: String, host: String?,
                        dt: String = "canary-dash",
                        hw: String = "") -> (service: String, txt: [String: String]) {
        var txt: [String: String] = ["device_id": deviceID, "name": deviceID, "dt": dt]
        if let host { txt["host"] = host }
        if !hw.isEmpty { txt["hw"] = hw }
        return (service: service, txt: txt)
    }

    /// One device, seen on several interfaces, is ONE row.
    func testInterfaceCopiesOfOneDeviceCollapse() {
        let rows = Discovery.rows(from: [
            advert(service: "canary_dash_001", deviceID: "canary_dash_001", host: "canary-dash-001-a1b2c3"),
            advert(service: "canary_dash_001", deviceID: "canary_dash_001", host: "canary-dash-001-a1b2c3"),
            advert(service: "canary_dash_001", deviceID: "canary_dash_001", host: "canary-dash-001-a1b2c3"),
        ])

        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows.first?.deviceID, "canary_dash_001")
    }

    /// TWO devices that share a published id are TWO rows.
    ///
    /// The regression this pins: `device_id` is a compile-time constant on
    /// firmware older than the personalized seed, so every Dash ever flashed
    /// answers to `canary_dash_001`. Keying the fold on it drops the second
    /// unit — and because FleetStore polls `discovery.found`, that device
    /// disappears from the fleet as well as from the discovery list. The
    /// hostnames differ because the firmware has always salted them.
    func testTwoLegacyDevicesSharingAnIDStayTwoRows() {
        let rows = Discovery.rows(from: [
            advert(service: "canary_dash_001", deviceID: "canary_dash_001", host: "canary-dash-001-a1b2c3"),
            advert(service: "canary_dash_001", deviceID: "canary_dash_001", host: "canary-dash-001-d4e5f6"),
        ])

        XCTAssertEqual(rows.count, 2, "two physical Dashes must not collapse into one")
        // Both are still reachable — the whole point, since an unlisted host
        // is a device nothing ever polls.
        XCTAssertEqual(Set(rows.compactMap(\.host)),
                       ["canary-dash-001-a1b2c3", "canary-dash-001-d4e5f6"])
        // They share what they CALL themselves and differ in row identity,
        // which is exactly the split the two fields exist for.
        XCTAssertEqual(Set(rows.map(\.deviceID)), ["canary_dash_001"])
        XCTAssertEqual(Set(rows.map(\.id)).count, 2, "row ids must be unique — ForEach depends on it")
    }

    /// A row that can be reached beats one that can't, whichever arrives
    /// second: not every interface carries the full TXT payload, and a row
    /// with no host is a row nothing can poll.
    func testACompleteAdvertIsNotReplacedByABareOne() {
        let withHost = advert(service: "canary_watch_001", deviceID: "canary_watch_001",
                              host: "canary-watch-001-aabbcc", dt: "canary-watch")
        var bare = withHost
        bare.txt.removeValue(forKey: "host")

        for order in [[withHost, bare], [bare, withHost]] {
            let rows = Discovery.rows(from: order)
            // The bare copy has no host, so it keys on the device id and
            // stands as its own row — but the reachable one must survive.
            XCTAssertTrue(rows.contains { $0.host == "canary-watch-001-aabbcc" },
                          "the advert we can actually reach must never be dropped")
        }
    }

    /// The advert carries what a row needs to draw itself and to be labeled.
    func testTheAdvertCarriesTypeAndBoard() {
        let rows = Discovery.rows(from: [
            advert(service: "canary_nightstand7_001", deviceID: "canary_nightstand7_001",
                   host: "canary-nightstand7-001-abc123",
                   dt: "canary-nightstand7", hw: "waveshare-esp32s3-lcd7"),
        ])

        let row = rows.first
        XCTAssertEqual(row?.publishedType, "canary-nightstand7")
        XCTAssertEqual(row?.hardware, "waveshare-esp32s3-lcd7")
        // Decodes as a display, so it is labeled one and offered its screen
        // controls — it used to fall through to .unknown and the generic bird.
        XCTAssertEqual(row?.deviceType, .display)
        // And it resolves to a real drawing, from the board.
        XCTAssertEqual(FleetFigure.resolve(deviceType: row?.deviceType ?? .unknown,
                                           published: row?.publishedType,
                                           hardware: row?.hardware)?.id,
                       "device.canary-display-dash7")
    }

    /// An advert with no TXT at all still becomes a row rather than being
    /// dropped — a Canary answering is worth showing.
    func testABareServiceStillBecomesARow() {
        let rows = Discovery.rows(from: [(service: "SCV-1A2B", txt: [:])])
        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows.first?.deviceID, "SCV-1A2B")
        XCTAssertEqual(rows.first?.id, "SCV-1A2B")
        XCTAssertEqual(rows.first?.deviceType, .unknown)
    }
}
