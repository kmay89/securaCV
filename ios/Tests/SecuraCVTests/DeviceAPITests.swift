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
