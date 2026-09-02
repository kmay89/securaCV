// DeviceAPITests.swift
//
// Host tests for DeviceAPI's pure edges — the parts that broke silently once:
//
//   * request-URL building: `appendingPathComponent` percent-encodes '?', so
//     a query baked into the path string turned /api/v1/witness?last=N into
//     the literal path /api/v1/witness%3Flast=N and the device answered 404 —
//     killing chain verification, the timeline, Verify-now, and the
//     head-watch, all behind `try?`. The test pins the URLComponents path.
//   * the SPKI-PEM → raw-key extraction that TOFU pinning depends on;
//   * the provisioning receipt's tolerance floor: a receipt without a token
//     must throw, not "pair" a device whose every call then 401s.

import XCTest
@testable import SecuraCV

final class DeviceAPITests: XCTestCase {

    private let base = URL(string: "http://canary-a3f7.local")!

    // ── request URLs keep their query separator ──
    func testWitnessURLKeepsTheQuerySeparator() {
        let url = DeviceAPI.requestURL(base: base, path: "/api/v1/witness",
                                       query: [URLQueryItem(name: "last", value: "20")])
        XCTAssertEqual(url.absoluteString, "http://canary-a3f7.local/api/v1/witness?last=20",
                       "the query must ride after a real '?', never a percent-encoded one")
    }

    func testPlainPathGainsNoQuestionMark() {
        let url = DeviceAPI.requestURL(base: base, path: "/api/v1/info", query: nil)
        XCTAssertEqual(url.absoluteString, "http://canary-a3f7.local/api/v1/info")
    }

    // ── SPKI PEM → raw 32-byte Ed25519 key (the TOFU pin's food) ──
    func testEd25519KeyExtractsFromSPKIPEM() {
        // A real `node:crypto` export — the exact shape the reference
        // device-api serves as public_key_pem in /api/v1/witness/export.
        let pem = """
        -----BEGIN PUBLIC KEY-----
        MCowBQYDK2VwAyEA/1xbPbWQyR4cv2opywtcrq/Lkfu4oL+cxTNjrGRVyFo=
        -----END PUBLIC KEY-----
        """
        let key = DeviceAPI.ed25519Key(fromSPKIPEM: pem)
        XCTAssertEqual(key?.count, 32, "SPKI DER is a fixed 12-byte prefix + the raw key")
        XCTAssertEqual(key?.first, 0xFF, "raw key starts right after the prefix (/1x… = 0xFF5C5B…)")
    }

    func testEd25519KeyRejectsNonEd25519PEM() {
        XCTAssertNil(DeviceAPI.ed25519Key(fromSPKIPEM: "not a pem at all"))
        // Valid base64, wrong shape (no SPKI prefix): must be rejected, not guessed at.
        let bogus = "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----"
        XCTAssertNil(DeviceAPI.ed25519Key(fromSPKIPEM: bogus))
    }

    // ── the "nothing phones home" gate ──
    // isPrivate is the ONLY thing between a pairing receipt / mDNS answer and an
    // outbound request. Its first version dropped non-numeric labels while
    // parsing, so "10.0.0.1.attacker.com" read as 10.0.0.1 and passed.
    private func isPrivate(_ s: String) -> Bool {
        guard let url = URL(string: s) else { return false }
        return DeviceAPI.isPrivate(url)
    }

    func testIsPrivateAcceptsTheAddressesACanaryCanHave() {
        for ok in ["http://192.168.1.7", "http://10.1.2.3:8080/api", "http://172.16.0.1",
                   "http://172.31.255.255", "http://127.0.0.1", "http://169.254.10.20",
                   "http://canary-a3f7.local", "http://Canary.LOCAL/api/fleet", "http://localhost:8080"] {
            XCTAssertTrue(isPrivate(ok), "\(ok) is a private/local host and must pass")
        }
    }

    func testIsPrivateRejectsPublicHostsAndLookalikes() {
        for bad in ["http://8.8.8.8", "http://172.32.0.1", "http://172.15.0.1", "http://11.0.0.1",
                    "http://10.0.0.1.attacker.com", "http://attacker.com.192.168.1.1",
                    "http://192.168.1.1.x", "http://192.168.1", "http://192.168.1.256",
                    "http://192.168.001.1234", "http://example.com", "https://securacv.com",
                    "http://.local", "http://local", "http://192.168.1.7.local.evil.com"] {
            XCTAssertFalse(isPrivate(bad), "\(bad) must NOT pass the private-host gate")
        }
    }

    func testDiscoveredHostBecomesALocalURL() {
        XCTAssertEqual(DeviceAPI.url(forDiscoveredHost: "canary-display-a1b2")?.absoluteString,
                       "http://canary-display-a1b2.local")
        XCTAssertEqual(DeviceAPI.url(forDiscoveredHost: " 192.168.1.5 ")?.absoluteString,
                       "http://192.168.1.5")
        XCTAssertEqual(DeviceAPI.url(forDiscoveredHost: "x.local")?.absoluteString, "http://x.local")
        XCTAssertNil(DeviceAPI.url(forDiscoveredHost: "   "))
        // And whatever discovery hands us still has to clear the gate.
        XCTAssertTrue(DeviceAPI.isPrivate(DeviceAPI.url(forDiscoveredHost: "canary-a3f7")!))
    }

    // ── the receipt's tolerance floor ──
    func testReceiptWithoutTokenThrows() {
        let json = Data(#"{"device_id":"canary-a3f7","base_url":"http://192.168.1.20"}"#.utf8)
        XCTAssertThrowsError(try JSONDecoder().decode(ProvisioningReceipt.self, from: json),
                             "a token-less receipt must fail loudly, not pair a device that can never authenticate")
    }

    func testReceiptWithTokenDecodes() throws {
        let json = Data(#"{"device_id":"canary-a3f7","base_url":"http://192.168.1.20","token":"cv_x"}"#.utf8)
        let receipt = try JSONDecoder().decode(ProvisioningReceipt.self, from: json)
        XCTAssertEqual(receipt.deviceID, "canary-a3f7")
        XCTAssertEqual(receipt.token, "cv_x")
    }
}
