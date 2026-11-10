// RefusedPairingsTests.swift
//
// A pairing whose token answers 403 is held until it is made again or
// removed, and its row asks for a new pairing instead of reading as dark.

import XCTest
@testable import SecuraCV

final class RefusedPairingsTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_800_000_000)

    private func ref(_ id: String, pairedAt: Date) -> PairedDeviceRef {
        PairedDeviceRef(id: id, name: "Porch", deviceType: .wap,
                        baseURL: URL(string: "http://192.168.1.20"), pairedAt: pairedAt)
    }

    func testOnlyA403IsARefusal() {
        XCTAssertTrue(RefusedPairings.isRefusal(DeviceError.http(403, "")))
        XCTAssertFalse(RefusedPairings.isRefusal(DeviceError.http(401, "")))
        XCTAssertFalse(RefusedPairings.isRefusal(DeviceError.http(429, "")),
                       "a held address is not a refused token")
        XCTAssertFalse(RefusedPairings.isRefusal(DeviceError.http(404, "")))
        XCTAssertFalse(RefusedPairings.isRefusal(DeviceError.notPairable))
        XCTAssertFalse(RefusedPairings.isRefusal(URLError(.timedOut)))
    }

    func testANotedPairingIsHeld() {
        var refused = RefusedPairings()
        XCTAssertTrue(refused.isEmpty)
        XCTAssertFalse(refused.holds(ref("a", pairedAt: t0)))
        refused.note(ref("a", pairedAt: t0))
        XCTAssertFalse(refused.isEmpty)
        XCTAssertTrue(refused.holds(ref("a", pairedAt: t0)))
        XCTAssertFalse(refused.holds(ref("b", pairedAt: t0)))
    }

    func testPairingAgainIsNotHeld() {
        var refused = RefusedPairings()
        refused.note(ref("a", pairedAt: t0))
        XCTAssertFalse(refused.holds(ref("a", pairedAt: t0.addingTimeInterval(60))))
    }

    func testPruneDropsRemovedAndRepairedPairings() {
        var refused = RefusedPairings()
        refused.note(ref("gone", pairedAt: t0))
        refused.note(ref("again", pairedAt: t0))
        refused.note(ref("kept", pairedAt: t0))
        refused.prune(to: [ref("again", pairedAt: t0.addingTimeInterval(60)),
                           ref("kept", pairedAt: t0)])
        XCTAssertFalse(refused.holds(ref("gone", pairedAt: t0)))
        XCTAssertFalse(refused.holds(ref("again", pairedAt: t0)))
        XCTAssertTrue(refused.holds(ref("kept", pairedAt: t0)))

        refused.prune(to: [])
        XCTAssertTrue(refused.isEmpty)
    }

    func testARefusedRowAsksForPairingAndIsNotDark() {
        var w = Witness(id: "a", deviceType: .wap, name: "Porch")
        XCTAssertEqual(w.displaySeverity, .ok)
        w.pairingRefused = true
        XCTAssertEqual(w.statusLine, "Needs pairing again")
        XCTAssertEqual(w.displaySeverity, .warn)
        XCTAssertFalse(w.link.isDark)
    }

    func testTamperStillOutranksARefusedPairing() {
        var w = Witness(id: "a", deviceType: .wap, name: "Porch")
        w.pairingRefused = true
        w.tamper = true
        XCTAssertEqual(w.displaySeverity, .tamper)
        XCTAssertEqual(w.statusLine, "Tamper detected")
    }
}
