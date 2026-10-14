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

    func testEveryCopysFieldsReachTheRowWhicheverCopyCameFirst() {
        // The location name, version and installation type ride whichever
        // interface copy carried them; the first private address still wins.
        let bare = (service: "homeassistant", txt: ["uuid": "abc",
                                                    "internal_url": "http://192.168.1.20:8123"])
        let full = (service: "homeassistant", txt: ["uuid": "abc", "location_name": "Home",
                                                    "version": "2026.10.1",
                                                    "installation_type": "Home Assistant OS",
                                                    "internal_url": "http://10.0.0.20:8123"])
        for (adverts, address) in [([bare, full], "http://192.168.1.20:8123"),
                                   ([full, bare], "http://10.0.0.20:8123")] {
            let rows = HubDiscoveryRows.rows(from: adverts)
            XCTAssertEqual(rows.count, 1)
            XCTAssertEqual(rows.first?.name, "Home")
            XCTAssertEqual(rows.first?.version, "2026.10.1")
            XCTAssertEqual(rows.first?.installationType, "Home Assistant OS")
            XCTAssertEqual(rows.first?.baseURL?.absoluteString, address)
        }
    }

    func testInterfaceCopiesMergeWhicheverOrderTheyArrive() {
        // The copy with the gated address and the copy with the metadata
        // arrive in either order; the row carries both either way, and the
        // first address that passed the gate is the one kept.
        let bare = (service: "Home", txt: ["uuid": "abc", "base_url": "https://my-ha.duckdns.org"])
        let full = (service: "Home", txt: ["uuid": "abc", "location_name": "Home", "version": "2026.10.1",
                                           "internal_url": "http://192.168.1.20:8123",
                                           "installation_type": "Home Assistant OS"])
        for adverts in [[bare, full], [full, bare]] {
            let rows = HubDiscoveryRows.rows(from: adverts)
            XCTAssertEqual(rows.count, 1)
            XCTAssertEqual(rows.first?.id, "abc")
            XCTAssertEqual(rows.first?.name, "Home")
            XCTAssertEqual(rows.first?.baseURL?.absoluteString, "http://192.168.1.20:8123")
            XCTAssertEqual(rows.first?.version, "2026.10.1")
            XCTAssertEqual(rows.first?.installationType, "Home Assistant OS")
        }
        let first = (service: "Home", txt: ["uuid": "abc", "internal_url": "http://192.168.1.20:8123"])
        let second = (service: "Home", txt: ["uuid": "abc", "internal_url": "http://10.0.0.5:8123"])
        XCTAssertEqual(HubDiscoveryRows.rows(from: [first, second]).first?.baseURL?.absoluteString,
                       "http://192.168.1.20:8123")
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
