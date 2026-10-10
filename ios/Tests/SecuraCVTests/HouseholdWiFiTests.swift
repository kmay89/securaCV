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

    func testEveryWiFiFormOpensWithTheRememberedNetworkButNeverOverTyping() {
        let home = HouseholdWiFi(ssid: "Home", password: "hunter22", savedAt: Date())
        XCTAssertEqual(HouseholdWiFiStore.prefill(typedSSID: "", remembered: home)?.ssid, "Home")
        XCTAssertEqual(HouseholdWiFiStore.prefill(typedSSID: "  ", remembered: home)?.password, "hunter22")
        XCTAssertNil(HouseholdWiFiStore.prefill(typedSSID: "Attic", remembered: home),
                     "what the person typed wins")
        XCTAssertNil(HouseholdWiFiStore.prefill(typedSSID: "", remembered: nil), "nothing remembered, nothing to fill")
    }

    func testUpdatingTheFleetWiFiReplacesOnlyAnOptedInProvenRecord() {
        let old = HouseholdWiFi(ssid: "Home", password: "old-pass", savedAt: Date())
        XCTAssertTrue(HouseholdWiFiStore.shouldReplace(existing: old, anyMoved: true, ssid: "Home"),
                      "a Canary came back on the new password: the next one gets it")
        XCTAssertFalse(HouseholdWiFiStore.shouldReplace(existing: old, anyMoved: false, ssid: "Home"),
                       "no Canary proved it: an unproven password is not remembered")
        XCTAssertFalse(HouseholdWiFiStore.shouldReplace(existing: nil, anyMoved: true, ssid: "Home"),
                       "the rollout is not an opt-in: nothing remembered stays nothing remembered")
        XCTAssertFalse(HouseholdWiFiStore.shouldReplace(existing: old, anyMoved: true, ssid: " "))
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
