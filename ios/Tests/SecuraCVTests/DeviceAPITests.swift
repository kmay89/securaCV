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
        XCTAssertNil(receipt.tlsCertFingerprint, "no tls_cert_fp → no pin")
    }

    // ── the receipt's TLS fingerprint (roadmap row 5) ──
    // canary_wap.ino send_provisioning_receipt writes `tls_cert_fp` as the
    // 64-hex SHA-256 of the certificate DER, or "" on an http-only device.
    private let fixtureFP = "cdfcca58db62ca948e8741d19e28ce1c79b4a818755559b419b1dd17c439dcfa"

    func testReceiptKeepsTheTLSFingerprint() throws {
        let json = Data(#"{"device_id":"canary-a3f7","base_url":"https://192.168.4.1","token":"cv_x","tls_cert_fp":"\#(fixtureFP)"}"#.utf8)
        let receipt = try JSONDecoder().decode(ProvisioningReceipt.self, from: json)
        XCTAssertEqual(receipt.tlsCertFingerprint, fixtureFP)
    }

    func testReceiptEmptyFingerprintIsNoPin() throws {
        // An http-only WAP writes "" — that is "no certificate", not a pin.
        let json = Data(#"{"device_id":"c","base_url":"http://192.168.4.1","token":"t","tls_cert_fp":""}"#.utf8)
        let receipt = try JSONDecoder().decode(ProvisioningReceipt.self, from: json)
        XCTAssertNil(receipt.tlsCertFingerprint)
    }

    // ── TLSPin: the hashing/compare helper behind PinnedTrustDelegate ──
    // A real self-signed Ed25519 certificate (DER, 325 bytes) minted with
    // `openssl req -x509 -newkey ed25519 … -outform DER`; fixtureFP above is
    // `sha256sum` of those bytes, computed on the host that minted it.
    private let fixtureDERBase64 =
        "MIIBQTCB9KADAgECAgEBMAUGAytlcDAgMR4wHAYDVQQDDBVzZWN1cmFjdi10ZXN0LWZpeHR1cmUwHhcNMjYwOTA4"
        + "MjEzODAxWhcNMzYwOTA1MjEzODAxWjAgMR4wHAYDVQQDDBVzZWN1cmFjdi10ZXN0LWZpeHR1cmUwKjAFBgMrZXAD"
        + "IQAr/seDrmDQ/U99fdEa0643C2Iqb9j2jiMAeX4k+2H6jaNTMFEwHQYDVR0OBBYEFKI3Aee8oBC1xb2/lTAGFpDL"
        + "sSjrMB8GA1UdIwQYMBaAFKI3Aee8oBC1xb2/lTAGFpDLsSjrMA8GA1UdEwEB/wQFMAMBAf8wBQYDK2VwA0EAW5LG"
        + "81Tdgou7cSeujQSQ18aNSUo8f95jxtexjLwPM/YMnbpjtn+oe0vT6trsuqfL6uqq13yX9C9XRuECWDU3AQ=="

    private var fixtureDER: Data { Data(base64Encoded: fixtureDERBase64)! }

    func testFingerprintOfDERMatchesTheHostHash() {
        XCTAssertEqual(fixtureDER.count, 325)
        XCTAssertEqual(TLSPin.fingerprintHex(ofDER: fixtureDER), fixtureFP)
    }

    func testPinMatchesExactlyAndTolerantly() {
        XCTAssertTrue(TLSPin.matches(der: fixtureDER, pinned: fixtureFP))
        // Case and separators are presentation, not identity.
        XCTAssertTrue(TLSPin.matches(der: fixtureDER, pinned: fixtureFP.uppercased()))
        let colons = stride(from: 0, to: 64, by: 2).map { i -> String in
            let start = fixtureFP.index(fixtureFP.startIndex, offsetBy: i)
            return String(fixtureFP[start..<fixtureFP.index(start, offsetBy: 2)])
        }.joined(separator: ":")
        XCTAssertTrue(TLSPin.matches(der: fixtureDER, pinned: colons))
    }

    func testPinRejectsAnyOtherCertificate() {
        var flipped = fixtureDER
        flipped[flipped.count - 1] ^= 0x01            // one bit of the signature
        XCTAssertFalse(TLSPin.matches(der: flipped, pinned: fixtureFP))
        XCTAssertFalse(TLSPin.matches(der: fixtureDER, pinned: String(repeating: "0", count: 64)))
        XCTAssertFalse(TLSPin.matches(der: fixtureDER, pinned: ""), "an empty pin never matches")
        XCTAssertFalse(TLSPin.matches(der: fixtureDER, pinned: String(fixtureFP.dropLast())))
    }

    func testNormalizeAcceptsOnlyA64HexPin() {
        XCTAssertEqual(TLSPin.normalize(fixtureFP), fixtureFP)
        XCTAssertEqual(TLSPin.normalize(" " + fixtureFP.uppercased() + "\n"), fixtureFP)
        XCTAssertNil(TLSPin.normalize(nil))
        XCTAssertNil(TLSPin.normalize(""))
        XCTAssertNil(TLSPin.normalize("not-a-fingerprint"))
        XCTAssertNil(TLSPin.normalize(String(repeating: "g", count: 64)), "g is not hex")
        XCTAssertNil(TLSPin.normalize(String(fixtureFP.dropLast(2))), "62 hex is not a SHA-256")
    }

    // ── DeviceAPI refuses an https Canary it cannot check ──
    func testHTTPSWithoutAPinIsRefused() {
        XCTAssertThrowsError(try DeviceAPI(base: URL(string: "https://192.168.1.20")!, token: "t")) { error in
            guard case DeviceError.tlsPinMissing = error else {
                return XCTFail("expected .tlsPinMissing, got \(error)")
            }
            XCTAssertFalse(error.localizedDescription.isEmpty, "the refusal names itself")
        }
        XCTAssertThrowsError(try DeviceAPI(base: URL(string: "https://192.168.1.20")!, token: "t",
                                           tlsFingerprint: ""))
    }

    func testHTTPSWithAPinAndPlainHTTPAreAccepted() {
        XCTAssertNoThrow(try DeviceAPI(base: URL(string: "https://192.168.1.20")!, token: "t",
                                       tlsFingerprint: fixtureFP))
        XCTAssertNoThrow(try DeviceAPI(base: URL(string: "http://192.168.1.20")!, token: "t"))
        // The private-host gate still comes first, pin or no pin.
        XCTAssertThrowsError(try DeviceAPI(base: URL(string: "https://securacv.com")!, token: "t",
                                           tlsFingerprint: fixtureFP))
    }

    func testIsTLSReadsTheScheme() {
        XCTAssertTrue(DeviceAPI.isTLS(URL(string: "https://192.168.1.20")!))
        XCTAssertTrue(DeviceAPI.isTLS(URL(string: "HTTPS://canary.local")!))
        XCTAssertFalse(DeviceAPI.isTLS(URL(string: "http://192.168.1.20")!))
    }
}
