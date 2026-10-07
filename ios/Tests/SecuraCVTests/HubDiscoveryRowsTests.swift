// HubDiscoveryRowsTests.swift
//
// Home Assistant's own advert, folded into rows the hub walkthrough can
// dial: the uuid as identity, the LAN address first, nothing off this
// network ever dialed, interface copies collapsed.

import XCTest
@testable import SecuraCV

final class HubDiscoveryRowsTests: XCTestCase {

    func testRowsPreferTheInternalAddressAndKeepTheUUIDAsIdentity() {
        let rows = HubDiscoveryRows.rows(from: [
            (service: "Home", txt: ["uuid": "abc", "location_name": "Home", "version": "2026.10.1",
                                    "base_url": "http://homeassistant.local:8123",
                                    "internal_url": "http://192.168.1.20:8123",
                                    "installation_type": "Home Assistant OS"]),
            (service: "Home", txt: ["uuid": "abc", "location_name": "Home", "version": "2026.10.1",
                                    "base_url": "http://homeassistant.local:8123",
                                    "internal_url": "http://192.168.1.20:8123"]),
        ])
        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows.first?.id, "abc")
        XCTAssertEqual(rows.first?.name, "Home")
        XCTAssertEqual(rows.first?.baseURL?.absoluteString, "http://192.168.1.20:8123")
        XCTAssertEqual(rows.first?.installationType, "Home Assistant OS")
    }

    func testAnAddressOffThisNetworkIsNeverDialed() {
        let rows = HubDiscoveryRows.rows(from: [
            (service: "Far", txt: ["base_url": "https://my-ha.duckdns.org", "internal_url": "https://my-ha.duckdns.org"]),
        ])
        XCTAssertEqual(rows.count, 1)
        XCTAssertNil(rows.first?.baseURL)
        XCTAssertEqual(rows.first?.id, "Far")
        XCTAssertEqual(rows.first?.name, "Far")
    }

    func testTheLANFallbackIsTakenWhenTheInternalAddressIsPublic() {
        let rows = HubDiscoveryRows.rows(from: [
            (service: "Home", txt: ["internal_url": "https://my-ha.duckdns.org", "base_url": "http://homeassistant.local:8123"]),
        ])
        XCTAssertEqual(rows.first?.baseURL?.absoluteString, "http://homeassistant.local:8123")
    }
}
