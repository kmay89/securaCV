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
}
