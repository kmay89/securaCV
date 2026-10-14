// SetupPortalTests.swift
//
// The Canary setup-network contract, pinned: the network name grammar,
// the exact form the portal's /join takes, the firmware's own limits, the
// /status vocabulary, and the WIFI: QR grammar a display draws.

import XCTest
@testable import SecuraCV

final class SetupPortalTests: XCTestCase {

    func testSetupNetworkNames() {
        XCTAssertTrue(SetupPortal.isSetupNetwork("SecuraCV-A1B2"))
        XCTAssertTrue(SetupPortal.isSetupNetwork("SecuraCV-A1B2-xy"))
        XCTAssertFalse(SetupPortal.isSetupNetwork("SecuraCV-A1B"))
        XCTAssertFalse(SetupPortal.isSetupNetwork("SecuraCV-A1B2-x"))
        XCTAssertFalse(SetupPortal.isSetupNetwork("securacv-A1B2"))
        XCTAssertFalse(SetupPortal.isSetupNetwork("MyHomeWiFi"))
    }

    func testJoinBodyIsTheFormThePortalPosts() {
        XCTAssertEqual(SetupPortal.joinBody(ssid: "Home Net", password: "p&ss", timeZone: "Europe/Berlin"),
                       "ssid=Home%20Net&pass=p%26ss&tz=Europe%2FBerlin")
        XCTAssertEqual(SetupPortal.joinBody(ssid: "A", password: "", timeZone: nil), "ssid=A&pass=")
    }

    func testCredentialLimitsAreTheFirmwaresOwn() {
        XCTAssertNil(SetupPortal.credentialProblem(ssid: "A", password: ""))
        XCTAssertNotNil(SetupPortal.credentialProblem(ssid: "", password: "x"))
        XCTAssertNotNil(SetupPortal.credentialProblem(ssid: String(repeating: "s", count: 33), password: ""))
        XCTAssertNil(SetupPortal.credentialProblem(ssid: String(repeating: "s", count: 32), password: String(repeating: "p", count: 64)))
        XCTAssertNotNil(SetupPortal.credentialProblem(ssid: "A", password: String(repeating: "p", count: 65)))
    }

    func testReplyAndStatusVocabulary() {
        XCTAssertTrue(SetupPortal.parseJoinReply(Data(#"{"ok":true}"#.utf8)).ok)
        let refused = SetupPortal.parseJoinReply(Data(#"{"ok":false,"reason":"bad request"}"#.utf8))
        XCTAssertFalse(refused.ok); XCTAssertEqual(refused.reason, "bad request")
        XCTAssertEqual(SetupPortal.parseStatus(Data(#"{"state":"idle"}"#.utf8)), .idle)
        XCTAssertEqual(SetupPortal.parseStatus(Data(#"{"state":"connecting"}"#.utf8)), .connecting)
        XCTAssertEqual(SetupPortal.parseStatus(Data(#"{"state":"success"}"#.utf8)), .success)
        XCTAssertEqual(SetupPortal.parseStatus(Data(#"{"state":"fail","reason":"wrong password"}"#.utf8)),
                       .fail(reason: "wrong password"))
        XCTAssertNil(SetupPortal.parseStatus(Data("<html>".utf8)))
    }

    func testWiFiQRGrammar() {
        XCTAssertEqual(SetupPortal.parseWiFiQR("WIFI:T:WPA;S:SecuraCV-A1B2;P:k3yk3yk3;;"),
                       SetupPortal.WiFiQR(ssid: "SecuraCV-A1B2", password: "k3yk3yk3", hidden: false))
        // Escapes, order, case and a hidden flag.
        XCTAssertEqual(SetupPortal.parseWiFiQR("wifi:s:My\\;Net\\:1;p:a\\\\b;h:true;t:WPA;;"),
                       SetupPortal.WiFiQR(ssid: "My;Net:1", password: "a\\b", hidden: true))
        XCTAssertNil(SetupPortal.parseWiFiQR("SCV1|s=x|p=y"))
        XCTAssertNil(SetupPortal.parseWiFiQR("WIFI:T:WPA;P:nossid;;"))
    }

    // MARK: - the Canary's own network list (GET /scan)

    func testTheScanReadsTheFirmwaresTwoShapes() throws {
        // setup_portal.cpp send_scan_json / canary-display provision.cpp.
        let body = Data(#"{"networks":[{"ssid":"Attic","rssi":-81,"secure":true},{"ssid":"Home","rssi":-52,"secure":true},{"ssid":"Cafe","rssi":-60,"secure":false},{"ssid":"Home","rssi":-70,"secure":true},{"ssid":"","rssi":-40,"secure":true}],"tz":"UTC"}"#.utf8)
        let parsed = try XCTUnwrap(SetupPortal.parseScan(body))
        XCTAssertFalse(parsed.scanning)
        XCTAssertEqual(parsed.networks.map(\.ssid), ["Home", "Cafe", "Attic"],
                       "strongest first, one row per name (its strongest), no nameless rows")
        XCTAssertEqual(parsed.networks.first?.rssi, -52)
        XCTAssertEqual(parsed.networks.first { $0.ssid == "Cafe" }?.secure, false)
        let sweeping = try XCTUnwrap(SetupPortal.parseScan(Data(#"{"scanning":true}"#.utf8)))
        XCTAssertTrue(sweeping.scanning, "still sweeping: ask again")
        XCTAssertNil(SetupPortal.parseScan(Data("not json".utf8)))
        XCTAssertNil(SetupPortal.parseScan(Data(#"{"ok":true}"#.utf8)))
    }

    func testTheRememberedNetworkIsPreselectedOnlyWhenTheCanaryCanSeeIt() {
        let nets = [SetupPortal.Network(ssid: "Home", rssi: -50, secure: true),
                    SetupPortal.Network(ssid: "Attic", rssi: -80, secure: true)]
        XCTAssertEqual(SetupPortal.preselect(nets, remembered: "Attic"), "Attic")
        XCTAssertEqual(SetupPortal.preselect(nets, remembered: "Home5G"), "Home", "else the strongest it heard")
        XCTAssertEqual(SetupPortal.preselect(nets, remembered: nil), "Home")
        XCTAssertNil(SetupPortal.preselect([], remembered: "Home"), "no list: the person types a name")
    }

    // MARK: - saying what went wrong

    func testEveryFirmwareVerdictGetsItsNextStep() {
        // wifi_join_policy.h's labels, with the tips setup_portal.cpp's own
        // page adds — the phone never shows a bare label.
        XCTAssertEqual(SetupPortal.advice(for: "Wrong password"),
                       "Wrong password — check for typos; it's case-sensitive.")
        XCTAssertTrue(SetupPortal.advice(for: "Network not found").contains("2.4 GHz"),
                      "a 5 GHz-only network is invisible to these boards")
        XCTAssertTrue(SetupPortal.advice(for: "No address from the router").contains("closer to your router"))
        XCTAssertTrue(SetupPortal.advice(for: "Couldn't connect").contains("closer to your router"))
        XCTAssertEqual(SetupPortal.advice(for: "Something new."), "Something new.", "an unknown reason is relayed as said")
        for reason in ["Wrong password", "Network not found", "No address from the router", "Couldn't connect", "x"] {
            XCTAssertTrue(SetupPortal.advice(for: reason).hasSuffix("."), reason)
        }
    }

    func testANetworkTheCanaryCannotSeeIsNamedBeforeSending() {
        XCTAssertNotNil(SetupPortal.notListedHint(ssid: "Home5G", listed: ["Home", "Attic"]))
        XCTAssertTrue(SetupPortal.notListedHint(ssid: "Home5G", listed: ["Home"])?.contains("2.4 GHz") ?? false)
        XCTAssertNil(SetupPortal.notListedHint(ssid: "Home", listed: ["Home"]))
        XCTAssertNil(SetupPortal.notListedHint(ssid: "Home5G", listed: []), "no list, nothing to judge by")
        XCTAssertNil(SetupPortal.notListedHint(ssid: "  ", listed: ["Home"]))
    }

    func testAPasswordThatCanNeverWorkIsNamedBeforeSending() {
        XCTAssertNotNil(SetupPortal.passwordHint("short", networkIsOpen: false))
        XCTAssertNil(SetupPortal.passwordHint("long enough", networkIsOpen: false))
        XCTAssertNil(SetupPortal.passwordHint("", networkIsOpen: false), "nothing typed yet is not an error")
        XCTAssertNil(SetupPortal.passwordHint("short", networkIsOpen: true), "an open network takes none")
    }
}
