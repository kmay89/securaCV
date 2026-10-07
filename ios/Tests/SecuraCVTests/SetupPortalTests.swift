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
}
