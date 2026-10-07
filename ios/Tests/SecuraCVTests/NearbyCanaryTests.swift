// NearbyCanaryTests.swift
//
// The card's policy: which heard devices count as "a new Canary nearby",
// what they are called, and when the card stays quiet. Pure, so every
// branch is a line here.

import XCTest
@testable import SecuraCV

final class NearbyCanaryTests: XCTestCase {

    private let now = Date(timeIntervalSince1970: 1_800_000_000)
    private let open = ImprovWire.ServiceData(state: .authorized, capabilities: 0x05)
    private let shut = ImprovWire.ServiceData(state: .awaitingAuthorization, capabilities: 0x05)

    private func heard(_ id: UUID = UUID(), name: String? = "Sense-AB12", beacon: FleetBeacon? = nil,
                       improv: Bool = true, data: ImprovWire.ServiceData? = nil, rssi: Int = -60,
                       ago: TimeInterval = 1) -> NearbyCanaries.Heard {
        NearbyCanaries.Heard(peripheralID: id, beacon: beacon, localName: name, improvAdvertised: improv,
                             improvServiceData: data ?? (improv ? open : nil), rssiDBM: rssi,
                             lastHeard: now.addingTimeInterval(-ago))
    }

    private func beacon(flags: UInt8, fp: (UInt8, UInt8) = (0xAB, 0x12)) -> FleetBeacon {
        FleetBeacon.parse(manufacturerData: FleetBeacon.encode(flags: flags, batteryPct: nil, healthPct: nil,
                                                               chainHeight: 1, fpB0: fp.0, fpB1: fp.1))!
    }

    func testTheNameGrammarIsStrict() {
        XCTAssertEqual(NearbyCanaries.parseName("Sense-AB12")?.family, .sense)
        XCTAssertEqual(NearbyCanaries.parseName("Sense-AB12")?.suffix, "AB12")
        XCTAssertEqual(NearbyCanaries.parseName("Vision-0F9C")?.family, .vision)
        XCTAssertEqual(NearbyCanaries.parseName("WAP-1234")?.family, .wap)
        XCTAssertEqual(NearbyCanaries.parseName("Canary-1234")?.family, .wap, "the flagship pairs like a WAP")
        XCTAssertNil(NearbyCanaries.parseName("SCV-AB12"), "the beacon's provisional name is not a door")
        XCTAssertNil(NearbyCanaries.parseName("Sense-ab12"), "lower case is not how the firmware spells it")
        XCTAssertNil(NearbyCanaries.parseName("Sense-AB1"))
        XCTAssertNil(NearbyCanaries.parseName("Dash-AB12"), "displays do not open a Bluetooth door")
        XCTAssertNil(NearbyCanaries.parseName("Sense"))
        XCTAssertNil(NearbyCanaries.parseName(""))
    }

    func testAnOpenDoorWithTheImprovServiceIsACandidate() {
        let c = NearbyCanaries.candidates(from: [heard()], pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertEqual(c.count, 1)
        XCTAssertEqual(c.first?.family, .sense)
        XCTAssertEqual(c.first?.suffix, "AB12")
        XCTAssertEqual(c.first?.title, "Radar witness AB12")
        XCTAssertEqual(c.first?.improvState, .authorized)
        XCTAssertTrue(c.first!.canIdentify && c.first!.canScanWiFi)
    }

    func testAShutDoorIsNotOffered() {
        let c = NearbyCanaries.candidates(from: [heard(data: shut)], pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertTrue(c.isEmpty, "awaiting authorization means the device will refuse credentials")
        let provisioned = ImprovWire.ServiceData(state: .provisioned, capabilities: 0)
        XCTAssertTrue(NearbyCanaries.candidates(from: [heard(data: provisioned)], pairedFingerprints: [],
                                                dismissedSuffixes: [], now: now).isEmpty)
    }

    func testTheBeaconBitAloneIsEnoughWhenTheScanResponseWasMissed() {
        let b = beacon(flags: FleetBeacon.flagSetupOpen)
        let c = NearbyCanaries.candidates(from: [heard(name: nil, beacon: b, improv: false)],
                                          pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertEqual(c.count, 1)
        XCTAssertNil(c.first?.family, "no name, no family claim")
        XCTAssertEqual(c.first?.suffix, "AB12", "the beacon's fingerprint suffix stands in")
        XCTAssertEqual(c.first?.displayName, "SCV-AB12")
        XCTAssertEqual(c.first?.title, "SCV-AB12")
        let quiet = beacon(flags: FleetBeacon.flagOnWiFiSTA)
        XCTAssertTrue(NearbyCanaries.candidates(from: [heard(name: nil, beacon: quiet, improv: false)],
                                                pairedFingerprints: [], dismissedSuffixes: [], now: now).isEmpty,
                      "a beacon with the door shut and no Improv service is a provisioned Canary minding its business")
    }

    func testAStaleSightingIsGone() {
        let c = NearbyCanaries.candidates(from: [heard(ago: NearbyCanaries.freshFor + 1)],
                                          pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertTrue(c.isEmpty)
        XCTAssertEqual(NearbyCanaries.candidates(from: [heard(ago: NearbyCanaries.freshFor - 1)],
                                                 pairedFingerprints: [], dismissedSuffixes: [], now: now).count, 1)
    }

    func testOurOwnCanaryWithItsDoorOpenIsNotNew() {
        let b = beacon(flags: FleetBeacon.flagSetupOpen)
        let mine = "0123456789abab12"   // ends in the beacon's suffix
        XCTAssertTrue(NearbyCanaries.candidates(from: [heard(beacon: b)], pairedFingerprints: [mine],
                                                dismissedSuffixes: [], now: now).isEmpty)
        // Two paired devices sharing the suffix: the match is ambiguous, so the
        // card still offers it rather than guessing which one it is.
        let twins = [mine, "ffffffffffffab12"]
        XCTAssertEqual(NearbyCanaries.candidates(from: [heard(beacon: b)], pairedFingerprints: twins,
                                                 dismissedSuffixes: [], now: now).count, 1)
        // A name without a beacon cannot be matched to a pairing: offered.
        XCTAssertEqual(NearbyCanaries.candidates(from: [heard()], pairedFingerprints: [mine],
                                                 dismissedSuffixes: [], now: now).count, 1)
    }

    func testADismissedSuffixStaysQuiet() {
        XCTAssertTrue(NearbyCanaries.candidates(from: [heard()], pairedFingerprints: [],
                                                dismissedSuffixes: ["AB12"], now: now).isEmpty)
    }

    func testStrongestFirstThenByName() {
        let far = heard(name: "Vision-0001", rssi: -80)
        let near = heard(name: "Sense-0002", rssi: -50)
        let same = heard(name: "Sense-0003", rssi: -50)
        let c = NearbyCanaries.candidates(from: [far, same, near], pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertEqual(c.map(\.displayName), ["Sense-0002", "Sense-0003", "Vision-0001"])
    }

    func testANamelessBeaconlessSightingIsNotShown() {
        // Improv advertised but neither a name nor a beacon to call it by.
        XCTAssertTrue(NearbyCanaries.candidates(from: [heard(name: nil)], pairedFingerprints: [],
                                                dismissedSuffixes: [], now: now).isEmpty)
    }
}
