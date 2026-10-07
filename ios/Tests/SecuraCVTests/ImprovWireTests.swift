// ImprovWireTests.swift
//
// The app's half of the Improv Wi-Fi sync guard. The firmware pins the same
// vectors in firmware/tests_host/test_improv_core.cpp; if the wire ever
// moves, that C test changes — and this one must fail until it changes with
// it. That is what keeps the phone and the Canary speaking the same bytes
// while both suites stay green.

import XCTest
@testable import SecuraCV

final class ImprovWireTests: XCTestCase {

    // improv-wifi.com/ble's worked example: `01 1E 0C {MyWirelessAP} 10
    // {mysecurepassword} CS` — 33 bytes, checksum 0xC0.
    private var specExample: [UInt8] {
        var v: [UInt8] = [0x01, 0x1E, 0x0C]
        v += Array("MyWirelessAP".utf8)
        v.append(0x10)
        v += Array("mysecurepassword".utf8)
        v.append(0xC0)
        return v
    }

    func testTheSpecExampleIsBuiltByteForByte() {
        let built = ImprovWire.wifiSettings(ssid: "MyWirelessAP", password: "mysecurepassword")
        XCTAssertEqual(built.map { [UInt8]($0) }, specExample)
        XCTAssertEqual(specExample.count, 33)
        XCTAssertEqual(ImprovWire.checksum(specExample.dropLast()), 0xC0)
    }

    func testAnOpenNetworkStillCarriesItsLengthByte() {
        let b = [UInt8](ImprovWire.wifiSettings(ssid: "Cafe", password: "")!)
        XCTAssertEqual(b.count, 3 + 1 + 4 + 1)
        XCTAssertEqual(b[1], 6)
        XCTAssertEqual(b[7], 0, "pass_len is zero")
    }

    func testOverSpecCredentialsAreRefusedNotTruncated() {
        let s32 = String(repeating: "s", count: 32), p64 = String(repeating: "p", count: 64)
        XCTAssertEqual(ImprovWire.wifiSettings(ssid: s32, password: p64)?.count, 101,
                       "32 + 64 is the longest legal frame")
        XCTAssertNil(ImprovWire.wifiSettings(ssid: s32 + "x", password: p64))
        XCTAssertNil(ImprovWire.wifiSettings(ssid: s32, password: p64 + "x"))
        XCTAssertNil(ImprovWire.wifiSettings(ssid: "", password: "pw"))
        XCTAssertNil(ImprovWire.wifiSettings(ssid: "a\u{0}b", password: ""), "a NUL inside is refused")
        // Bytes, not characters: a 20-character SSID of 3-byte glyphs is 60 bytes.
        XCTAssertNil(ImprovWire.wifiSettings(ssid: String(repeating: "家", count: 20), password: ""))
    }

    func testTheArgumentlessCommandsAreThreeBytes() {
        XCTAssertEqual([UInt8](ImprovWire.identify), [0x02, 0x00, 0x02])
        XCTAssertEqual([UInt8](ImprovWire.deviceInfo), [0x03, 0x00, 0x03])
        XCTAssertEqual([UInt8](ImprovWire.wifiNetworks), [0x04, 0x00, 0x04])
        XCTAssertNil(ImprovWire.command(.identify, data: [UInt8](repeating: 0, count: 256)))
    }

    func testAResultFramesItsStringsAndReadsBack() {
        // The same frame the firmware test builds: a URL and an empty string.
        let url = "http://canary-sense-ab12.local/"
        var frame: [UInt8] = [0x01, UInt8(1 + url.utf8.count + 1), UInt8(url.utf8.count)]
        frame += Array(url.utf8)
        frame.append(0)
        frame.append(ImprovWire.checksum(frame))
        let r = ImprovWire.parseResult(Data(frame))
        XCTAssertEqual(r, ImprovWire.Result(command: 0x01, strings: [url, ""]))

        // The empty result that closes a networks scan: 04 00 04.
        XCTAssertEqual(ImprovWire.parseResult(Data([0x04, 0x00, 0x04])),
                       ImprovWire.Result(command: 0x04, strings: []))
    }

    func testANetworkRowIsThreeStrings() {
        var frame: [UInt8] = [0x04, 0x00]
        for s in ["Home", "-61", "WPA2"] {
            frame.append(UInt8(s.utf8.count)); frame += Array(s.utf8)
        }
        frame[1] = UInt8(frame.count - 2)
        frame.append(ImprovWire.checksum(frame))
        let r = ImprovWire.parseResult(Data(frame))!
        XCTAssertEqual(ImprovWire.network(from: r.strings),
                       ImprovWire.Network(ssid: "Home", rssi: -61, auth: "WPA2"))
        XCTAssertNil(ImprovWire.network(from: ["Home", "loud", "WPA2"]), "rssi must be a number")
        XCTAssertNil(ImprovWire.network(from: ["", "-61", "WPA2"]), "a nameless row is not a network")
        XCTAssertTrue(ImprovWire.Network(ssid: "Cafe", rssi: -50, auth: "NO").isOpen)
        XCTAssertFalse(ImprovWire.Network(ssid: "Home", rssi: -50, auth: "WPA3").isOpen)
    }

    func testACorruptResultIsNotWalked() {
        var frame: [UInt8] = [0x01, 0x04, 0x03] + Array("abc".utf8)
        frame.append(ImprovWire.checksum(frame))
        XCTAssertNotNil(ImprovWire.parseResult(Data(frame)))
        var overrun = frame; overrun[2] = 9
        overrun[overrun.count - 1] = ImprovWire.checksum(overrun.dropLast())
        XCTAssertNil(ImprovWire.parseResult(Data(overrun)), "inner length past the end")
        var badSum = frame; badSum[badSum.count - 1] ^= 0xFF
        XCTAssertNil(ImprovWire.parseResult(Data(badSum)), "bad checksum")
        var badLen = frame; badLen[1] = 3
        badLen[badLen.count - 1] = ImprovWire.checksum(badLen.dropLast())
        XCTAssertNil(ImprovWire.parseResult(Data(badLen)), "data_len disagrees")
        XCTAssertNil(ImprovWire.parseResult(Data([0x01, 0x00])), "two bytes is not a result")
    }

    func testServiceDataIsStateAndCapabilities() {
        let sd = ImprovWire.parseServiceData(Data([0x02, 0x05, 0, 0, 0, 0]))
        XCTAssertEqual(sd, ImprovWire.ServiceData(state: .authorized, capabilities: 0x05))
        XCTAssertTrue(sd!.canIdentify && sd!.canScanWiFi)
        XCTAssertTrue(sd!.state.isOpen)
        XCTAssertFalse(ImprovWire.parseServiceData(Data([0x01, 0x00, 0, 0, 0, 0]))!.state.isOpen,
                       "awaiting authorization is a shut door")
        XCTAssertNil(ImprovWire.parseServiceData(Data([0x09, 0x00, 0, 0, 0, 0])), "an unknown state byte")
        XCTAssertNil(ImprovWire.parseServiceData(Data([0x02])), "one byte is not service data")
        XCTAssertEqual(ImprovWire.serviceDataUUID16, 0x4677)
    }

    func testStateAndErrorBytesDecode() {
        XCTAssertEqual(ImprovWire.parseState(Data([0x04])), .provisioned)
        XCTAssertNil(ImprovWire.parseState(Data()))
        XCTAssertEqual(ImprovWire.parseError(Data([0x03])), .unableToConnect)
        XCTAssertEqual(ImprovWire.parseError(Data([0xFF])), .unknown)
        XCTAssertNil(ImprovWire.parseError(Data([0x42])), "a byte outside the vocabulary")
        for e in [ImprovWire.Error.invalidRPC, .unknownRPC, .unableToConnect, .notAuthorized, .badHostname, .unknown] {
            XCTAssertFalse(e.message.isEmpty, "\(e) has words for the person")
        }
        XCTAssertTrue(ImprovWire.Error.none.message.isEmpty)
    }

    func testTheUUIDsAreTheStandards() {
        XCTAssertEqual(ImprovWire.serviceUUID, "00467768-6228-2272-4663-277478268000")
        for (i, u) in [ImprovWire.currentStateUUID, ImprovWire.errorStateUUID, ImprovWire.rpcCommandUUID,
                       ImprovWire.rpcResultUUID, ImprovWire.capabilitiesUUID].enumerated() {
            XCTAssertEqual(u, "00467768-6228-2272-4663-27747826800\(i + 1)")
        }
    }

    // ── the claim ticket (SecuraCV's companion service, a WAP only) ──
    // The JSON canary-wap writes to CLAIM for the link that provisioned,
    // and "{}" for everyone else (firmware/common/network/claim_ticket.h).

    private let claimHex = "0123456789abcdef0123456789abcdef"
    private let claimFP = String(repeating: "ab", count: 32)

    /// The firmware's body, with fields replaced (a value) or removed (nil).
    private func claimBody(_ overrides: [String: Any?] = [:]) -> Data {
        var obj: [String: Any] = [
            "device_id": "wap-ab12",
            "claim": claimHex,
            "claim_url": "https://canary-ab12.local/api/provisioning-receipt?claim=\(claimHex)",
            "tls_cert_fp": claimFP,
            "sta_ip": "192.168.1.23",
            "mdns_host": "canary-ab12",
            "expires_in_s": 180,
        ]
        for (key, value) in overrides {
            if let value {
                obj[key] = value
            } else {
                obj.removeValue(forKey: key)
            }
        }
        return try! JSONSerialization.data(withJSONObject: obj)
    }

    func testAClaimReadDecodesEveryField() throws {
        let c = try XCTUnwrap(ImprovWire.parseClaim(claimBody()))
        XCTAssertEqual(c.deviceID, "wap-ab12")
        XCTAssertEqual(c.claim, claimHex)
        XCTAssertEqual(c.claimURL?.scheme, "https")
        XCTAssertEqual(c.claimURL?.host, "canary-ab12.local")
        XCTAssertEqual(c.claimURL?.path, "/api/provisioning-receipt")
        XCTAssertEqual(c.claimURL?.query, "claim=\(claimHex)")
        XCTAssertEqual(c.tlsCertFingerprint, claimFP)
        XCTAssertEqual(c.staIP, "192.168.1.23")
        XCTAssertEqual(c.mdnsHost, "canary-ab12")
        XCTAssertEqual(c.expiresIn, 180)
        XCTAssertEqual(c, ImprovWire.parseClaim(claimBody()), "Equatable, for the plan's tests")
        XCTAssertEqual(c.hashValue, ImprovWire.parseClaim(claimBody())!.hashValue)
    }

    func testNotYoursIsNil() {
        XCTAssertNil(ImprovWire.parseClaim(Data("{}".utf8)),
                     "the door's answer to anyone but the link that provisioned, and to a second read")
        XCTAssertNil(ImprovWire.parseClaim(Data()))
        XCTAssertNil(ImprovWire.parseClaim(Data("[]".utf8)))
        XCTAssertNil(ImprovWire.parseClaim(Data("not json".utf8)))
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["device_id": nil])), "no device id, no pairing")
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["device_id": ""])))
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": nil])))
    }

    func testAClaimIsExactly32HexCharacters() {
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": String(claimHex.dropLast())])), "31")
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": claimHex + "0"])), "33")
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": "g" + String(claimHex.dropFirst())])), "g is not hex")
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": ""])))
        XCTAssertNil(ImprovWire.parseClaim(claimBody(["claim": 12345])), "a number is not a claim")
        // Either case is accepted (the firmware's compare folds it) and
        // folded to the firmware's own lower-case spelling.
        XCTAssertEqual(ImprovWire.parseClaim(claimBody(["claim": claimHex.uppercased()]))?.claim, claimHex)
        XCTAssertEqual(ImprovWire.claimHexLength, 32)
    }

    func testAnEmptyFingerprintIsNoPin() throws {
        let http = try XCTUnwrap(ImprovWire.parseClaim(claimBody(["tls_cert_fp": ""])),
                                 "an http WAP writes the empty string, and the claim still decodes")
        XCTAssertNil(http.tlsCertFingerprint)
        XCTAssertNil(try XCTUnwrap(ImprovWire.parseClaim(claimBody(["tls_cert_fp": nil]))).tlsCertFingerprint)
    }

    func testTheClaimURLMustBeAnHTTPURLWithAHost() throws {
        XCTAssertNil(try XCTUnwrap(ImprovWire.parseClaim(claimBody(["claim_url": nil]))).claimURL)
        XCTAssertNil(try XCTUnwrap(ImprovWire.parseClaim(claimBody(["claim_url": ""]))).claimURL)
        XCTAssertNil(try XCTUnwrap(ImprovWire.parseClaim(claimBody(["claim_url": "ftp://canary-ab12.local/x"]))).claimURL)
        XCTAssertNil(try XCTUnwrap(ImprovWire.parseClaim(claimBody(["claim_url": "/api/provisioning-receipt?claim=x"]))).claimURL,
                     "no host")
        let byIP = try XCTUnwrap(ImprovWire.parseClaim(claimBody(
            ["claim_url": "http://192.168.1.23/api/provisioning-receipt?claim=\(claimHex)"])))
        XCTAssertEqual(byIP.claimURL?.host, "192.168.1.23")
        XCTAssertEqual(byIP.claimURL?.scheme, "http")
    }

    func testTheClaimURLHostCanBeSwappedForTheAddress() throws {
        let c = try XCTUnwrap(ImprovWire.parseClaim(claimBody()))
        let viaIP = try XCTUnwrap(ImprovWire.claimURL(c, host: "192.168.1.23"))
        XCTAssertEqual(viaIP.absoluteString, "https://192.168.1.23/api/provisioning-receipt?claim=\(claimHex)",
                       "scheme, path and query stay; only the host moves")
        XCTAssertEqual(ImprovWire.claimURL(c, host: nil), c.claimURL, "no host: the URL as given")
        XCTAssertEqual(ImprovWire.claimURL(c, host: " "), c.claimURL)
        // A port rides with the host.
        let ported = try XCTUnwrap(ImprovWire.parseClaim(claimBody(
            ["claim_url": "http://canary-ab12.local:8080/api/provisioning-receipt?claim=\(claimHex)"])))
        XCTAssertEqual(ImprovWire.claimURL(ported, host: "10.0.0.9")?.absoluteString,
                       "http://10.0.0.9:8080/api/provisioning-receipt?claim=\(claimHex)")
        // No claim URL, nothing to rebuild.
        let bare = try XCTUnwrap(ImprovWire.parseClaim(claimBody(["claim_url": nil])))
        XCTAssertNil(ImprovWire.claimURL(bare, host: "192.168.1.23"))
    }

    func testTheCompanionServiceIsSecuraCVsOutsideTheImprovService() {
        XCTAssertEqual(ImprovWire.companionServiceUUID, "8fc1cf00-b162-4401-9607-c8ac21383e90")
        XCTAssertEqual(ImprovWire.claimUUID, "8fc1cf01-b162-4401-9607-c8ac21383e90")
        XCTAssertFalse(ImprovWire.companionServiceUUID.hasPrefix("00467768"),
                       "a separate service beside the Improv one, not a sixth characteristic in it")
        XCTAssertEqual(String(ImprovWire.claimUUID.dropFirst(8)), String(ImprovWire.companionServiceUUID.dropFirst(8)),
                       "one base UUID")
    }

    func testTheDoorRulesHaveWordsForThePerson() {
        let shut = ImprovWire.Error.notAuthorized.message
        XCTAssertTrue(shut.contains("BOOT button"), "the tap is the way back in on a Sense or Vision")
        XCTAssertTrue(shut.contains("power-cycle"), "and the power cycle on a brand-new one")
        XCTAssertTrue(shut.contains("WAP"), "a WAP has no tap door yet")
        XCTAssertTrue(ImprovWire.Error.invalidRPC.message.contains("wait a moment"), "the 3 s cooldown between writes")
    }
}
