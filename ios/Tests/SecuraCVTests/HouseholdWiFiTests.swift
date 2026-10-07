// HouseholdWiFiTests.swift
//
// The one secret the nearby-Canary sheet may keep: when a join is worth
// remembering (never an unproven password), and the custody the store
// leans on — the Keychain, device-bound, never synced. The Keychain itself
// is the host's business in an unsigned CI build (UnsealFlowTests says
// why), so the custody half is pinned at the source: the attributes
// Keychain.set writes, read off the file the way the fixture suites read
// theirs by #filePath.

import XCTest
@testable import SecuraCV

final class HouseholdWiFiTests: XCTestCase {

    func testOnlyAProvenJoinWithTheToggleOnIsRemembered() {
        XCTAssertTrue(HouseholdWiFiStore.shouldRemember(toggleOn: true, joined: true, ssid: "Home"))
        XCTAssertFalse(HouseholdWiFiStore.shouldRemember(toggleOn: false, joined: true, ssid: "Home"),
                       "the person said no")
        XCTAssertFalse(HouseholdWiFiStore.shouldRemember(toggleOn: true, joined: false, ssid: "Home"),
                       "an unproven password is not a password worth remembering")
        XCTAssertFalse(HouseholdWiFiStore.shouldRemember(toggleOn: true, joined: true, ssid: ""),
                       "never a nameless network")
        XCTAssertFalse(HouseholdWiFiStore.shouldRemember(toggleOn: true, joined: true, ssid: "   "))
        XCTAssertFalse(HouseholdWiFiStore.shouldRemember(toggleOn: false, joined: false, ssid: ""))
        XCTAssertTrue(HouseholdWiFiStore.shouldRemember(toggleOn: true, joined: true, ssid: " Home "),
                      "the sheet trims the name; so does the rule")
    }

    func testTheRecordRoundTripsAsJSON() throws {
        let saved = HouseholdWiFi(ssid: "Home", password: "hunter2",
                                  savedAt: Date(timeIntervalSince1970: 1_800_000_000))
        let data = try JSONEncoder().encode(saved)
        XCTAssertEqual(try JSONDecoder().decode(HouseholdWiFi.self, from: data), saved)
    }

    /// #filePath → ios/Tests/SecuraCVTests/… → the ios tree is three up.
    /// Skips (rather than fails) when the checkout isn't visible from the
    /// test host.
    private func source(_ relative: String) throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .appendingPathComponent(relative)
        try XCTSkipUnless(FileManager.default.fileExists(atPath: url.path),
                          "ios checkout not visible from the test host")
        return try String(contentsOf: url, encoding: .utf8)
    }

    func testTheKeychainItemIsDeviceBoundAndNeverSynced() throws {
        // Keychain.set is the one writer every store here goes through.
        let keychain = try source("Sources/SecuraCV/Security/Keychain.swift")
        XCTAssertTrue(keychain.contains("kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly"),
                      "ThisDeviceOnly: the item never rides iCloud Keychain")
        XCTAssertFalse(keychain.contains("kSecAttrSynchronizable"),
                       "nothing opts an item into iCloud Keychain sync")
        XCTAssertFalse(keychain.contains("kSecAttrAccessibleAlways"),
                       "nor the always-readable class")
        // And the household Wi-Fi goes through it — never UserDefaults.
        let store = try source("Sources/SecuraCV/Security/HouseholdWiFi.swift")
        XCTAssertTrue(store.contains("Keychain.set(") && store.contains("Keychain.get(")
                      && store.contains("Keychain.delete("))
        XCTAssertFalse(store.contains("UserDefaults"), "a Wi-Fi password is a secret, not a preference")
    }
}
