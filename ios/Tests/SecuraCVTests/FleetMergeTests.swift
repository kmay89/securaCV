// FleetMergeTests.swift
//
// The precedence rules in FleetMerge are a security boundary, not a style
// choice: they are what stops a coarse, unauthenticated advert from painting
// trust onto a device nobody verified. Each test below names the rule it
// guards, so a future change that "simplifies" the merge fails loudly.

import XCTest
@testable import SecuraCV

final class FleetMergeTests: XCTestCase {

    private func sighting(flags: UInt8 = 0, battery: Int? = nil, health: Int? = nil,
                          chain: UInt32 = 0, fpB0: UInt8 = 0xAB, fpB1: UInt8 = 0xCD,
                          rssi: Int = -55, at date: Date = Date(),
                          id: UUID = UUID(), name: String? = nil) -> BeaconSighting {
        let beacon = FleetBeacon.parse(manufacturerData:
            FleetBeacon.encode(flags: flags, batteryPct: battery, healthPct: health,
                               chainHeight: chain, fpB0: fpB0, fpB1: fpB1))!
        return BeaconSighting(beacon: beacon, rssiDBM: rssi, lastHeard: date,
                              peripheralID: id, localName: name)
    }

    // ── Rule: coarse never manufactures trust ──

    func testBeaconNeverSetsATrustBadge() {
        var w = Witness(id: "canary-a3f7")
        FleetMerge.fold(sighting(chain: 900), into: &w)
        XCTAssertEqual(w.badge, .unknown, "a beacon can never earn a trust badge")
    }

    func testSelfReportClaimingOkNeverSetsATrustBadge() {
        var w = Witness(id: "canary-a3f7")
        let row = FleetSelfDevice(name: "Front Door", online: true, chain: "ok",
                                  product: "canary-wap", chainHeight: 12)
        FleetMerge.fold(row, into: &w)
        XCTAssertEqual(w.badge, .unknown,
                       #"a device SAYING "chain":"ok" is a claim; only ChainVerifier sets the badge"#)
    }

    func testMergeNeverDowngradesAVerifiedBadge() {
        var w = Witness(id: "canary-a3f7")
        w.badge = .verified
        FleetMerge.fold(sighting(flags: FleetBeacon.flagDegraded), into: &w)
        FleetMerge.fold(FleetSelfDevice(name: "x", online: true, chain: "unknown", product: "canary-wap"),
                        into: &w)
        XCTAssertEqual(w.badge, .verified, "coarse tiers must not touch a verified badge either way")
    }

    // ── Rule: safety-positive facts only ever go up ──

    func testBeaconRaisesTamperButNeverClearsIt() {
        var w = Witness(id: "canary-a3f7")
        FleetMerge.fold(sighting(flags: FleetBeacon.flagTamper), into: &w)
        XCTAssertTrue(w.tamper, "a tamper flag on the wire raises tamper")

        // A later beacon with the flag clear must NOT clear the condition:
        // silence is not evidence the tamper ended.
        FleetMerge.fold(sighting(flags: 0), into: &w)
        XCTAssertTrue(w.tamper, "a beacon that stops mentioning tamper must not clear it")
    }

    // ── Rule: fill gaps, never replace a stronger tier's value ──

    func testBeaconFillsBatteryOnlyWhenUnknown() {
        var known = Witness(id: "a")
        known.batteryPct = 90                       // from /api/v1/info
        FleetMerge.fold(sighting(battery: 12), into: &known)
        XCTAssertEqual(known.batteryPct, 90, "HTTP's reading wins over the beacon's")

        var unknown = Witness(id: "b")
        FleetMerge.fold(sighting(battery: 12), into: &unknown)
        XCTAssertEqual(unknown.batteryPct, 12, "with nothing known, the beacon fills the gap")
    }

    /// The wire carries only the low 16 bits. Writing that over a real height
    /// would silently truncate a chain that had grown past 65535.
    func testTruncatedChainHeightNeverOverwritesAFullOne() {
        var w = Witness(id: "a")
        w.chainLength = 70_000
        FleetMerge.fold(sighting(chain: 70_000), into: &w)
        XCTAssertEqual(w.chainLength, 70_000, "a 16-bit height must not truncate a known full height")

        var fresh = Witness(id: "b")
        FleetMerge.fold(sighting(chain: 1234), into: &fresh)
        XCTAssertEqual(fresh.chainLength, 1234, "with nothing known, the low bits are better than zero")
    }

    /// The Witness field is the device's *Wi-Fi* RSSI. A BLE RSSI is a different
    /// radio at a different power — conflating them makes the signal bars lie.
    func testBeaconDoesNotTouchWiFiRSSI() {
        var w = Witness(id: "a")
        FleetMerge.fold(sighting(rssi: -40), into: &w)
        XCTAssertNil(w.rssiDBM, "a BLE RSSI must never be shown as the device's Wi-Fi RSSI")
    }

    // ── Rule: being heard is proof of life ──

    func testHearingADarkDeviceBringsItBack() {
        var w = Witness(id: "a")
        w.link = .lost                              // HTTP gave up on it
        FleetMerge.fold(sighting(), into: &w)
        XCTAssertEqual(w.link, .online, "if we can hear it, it is alive on some radio")
        XCTAssertTrue(w.seenViaBLE)
        XCTAssertNotNil(w.lastSeen)
    }

    func testLastSeenOnlyMovesForward() {
        let old = Date(timeIntervalSince1970: 1_000_000)
        let new = Date(timeIntervalSince1970: 2_000_000)
        var w = Witness(id: "a")
        w.lastSeen = new
        FleetMerge.fold(sighting(at: old), into: &w)
        XCTAssertEqual(w.lastSeen, new, "an older sighting must not rewind lastSeen")
    }

    func testSelfReportSayingOfflineOnlyBeatsUnknown() {
        let down = FleetSelfDevice(name: "x", online: false, chain: "unknown", product: "canary-wap")

        var unknown = Witness(id: "a")                       // no idea yet
        FleetMerge.fold(down, into: &unknown)
        XCTAssertEqual(unknown.link, .offline, "'I am down' beats 'no idea'")

        var reached = Witness(id: "b")
        reached.link = .online                                // we just polled it
        FleetMerge.fold(down, into: &reached)
        XCTAssertEqual(reached.link, .online, "a stale self-report must not un-see a live poll")
    }

    // ── Rule: "seeing" is present tense — folded live, aged out by time ──

    private func v2Sighting(detectClass: UInt8, score: Int?, at date: Date = Date(),
                            id: UUID = UUID()) -> BeaconSighting {
        let beacon = FleetBeacon.parse(manufacturerData:
            FleetBeacon.encodeV2(flags: 0, batteryPct: nil, healthPct: nil,
                                 chainHeight: 0, fpB0: 0xAB, fpB1: 0xCD,
                                 detectClass: detectClass, detectScore: score))!
        return BeaconSighting(beacon: beacon, rssiDBM: -55, lastHeard: date,
                              peripheralID: id, localName: nil)
    }

    func testV2DetectionFoldsAsLiveSeeingState() {
        let heard = Date()
        var w = Witness(id: "a")
        FleetMerge.fold(v2Sighting(detectClass: FleetBeacon.detectPerson, score: 87, at: heard),
                        into: &w)
        XCTAssertEqual(w.seeingClass, .person)
        XCTAssertEqual(w.seeingScore, 87)
        XCTAssertEqual(w.seeingAt, heard)

        // A fresher claim replaces an older one — this is live state, the
        // sender is the only authority, not a fill-the-gap field.
        FleetMerge.fold(v2Sighting(detectClass: FleetBeacon.detectVehicle, score: nil), into: &w)
        XCTAssertEqual(w.seeingClass, .vehicle)
        XCTAssertNil(w.seeingScore, "an unscored claim must not inherit the old score")
    }

    func testSilenceNeverClearsSeeingStateOnFold() {
        // A v1 beacon (and a v2 "none") carry no claim — neither may clear
        // one. Clearing is time's job, through Witness.seeingNow.
        var w = Witness(id: "a")
        FleetMerge.fold(v2Sighting(detectClass: FleetBeacon.detectPackage, score: 60), into: &w)
        FleetMerge.fold(sighting(), into: &w)                                   // v1
        FleetMerge.fold(v2Sighting(detectClass: FleetBeacon.detectNone, score: nil), into: &w)
        XCTAssertEqual(w.seeingClass, .package, "silence on the wire is not evidence the seeing ended")
    }

    func testUnknownFutureDetectionClassFoldsNothing() {
        var w = Witness(id: "a")
        FleetMerge.fold(v2Sighting(detectClass: 0x7F, score: 50), into: &w)
        XCTAssertNil(w.seeingClass, "a class this build has never heard of renders as nothing, not a guess")
    }

    func testSeeingNowAgesOutInsteadOfGoingStale() {
        let heard = Date()
        var w = Witness(id: "a")
        FleetMerge.fold(v2Sighting(detectClass: FleetBeacon.detectAnimal, score: 42, at: heard),
                        into: &w)

        let fresh = w.seeingNow(asOf: heard.addingTimeInterval(Witness.seeingFreshness - 1))
        XCTAssertEqual(fresh?.kind, .animal)
        XCTAssertEqual(fresh?.score, 42)

        XCTAssertNil(w.seeingNow(asOf: heard.addingTimeInterval(Witness.seeingFreshness + 1)),
                     #"a quiet beacon must read as silence, never a stale "Seeing animal""#)
    }

    // ── attach(): matching heard beacons to known Canaries ──

    func testAttachFoldsIntoTheMatchingWitnessByFingerprint() {
        var fleet = [Witness(id: "canary-a3f7")]
        fleet[0].fingerprint = "0011223344556677889900aabbccabcd"
        FleetMerge.attach([sighting(battery: 55, fpB0: 0xAB, fpB1: 0xCD)], to: &fleet)

        XCTAssertEqual(fleet.count, 1, "a matched beacon decorates the existing row, never duplicates it")
        XCTAssertEqual(fleet[0].batteryPct, 55)
        XCTAssertTrue(fleet[0].seenViaBLE)
    }

    func testAttachSurfacesUnknownCanariesAsProvisionalRows() {
        var fleet: [Witness] = []
        FleetMerge.attach([sighting(fpB0: 0x1A, fpB1: 0x2B, name: nil)], to: &fleet)

        XCTAssertEqual(fleet.count, 1)
        XCTAssertEqual(fleet[0].name, "SCV-1A2B", "an unpaired Canary shows under its fingerprint name")
        XCTAssertTrue(fleet[0].id.hasPrefix("ble:"),
                      "provisional ids are namespaced so they can't collide with real or demo ids")
    }

    /// Two bytes narrow the field; they don't prove identity. If a suffix could
    /// belong to two known Canaries, decorating either one would be a guess.
    func testAmbiguousFingerprintDecoratesNobody() {
        var fleet = [Witness(id: "canary-1"), Witness(id: "canary-2")]
        fleet[0].fingerprint = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaabcd"
        fleet[1].fingerprint = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbabcd"

        FleetMerge.attach([sighting(battery: 33, fpB0: 0xAB, fpB1: 0xCD)], to: &fleet)

        XCTAssertEqual(fleet.count, 3, "an ambiguous beacon stands alone rather than mislabel a Canary")
        XCTAssertNil(fleet[0].batteryPct)
        XCTAssertNil(fleet[1].batteryPct)
        XCTAssertEqual(fleet[2].batteryPct, 33)
    }

    /// The refresh loop runs every 20s; a device heard repeatedly must not
    /// accumulate a row per sighting.
    func testRepeatedSightingsOfTheSameDeviceStayOneRow() {
        let id = UUID()
        var fleet: [Witness] = []
        FleetMerge.attach([sighting(battery: 10, id: id)], to: &fleet)
        FleetMerge.attach([sighting(battery: 10, id: id)], to: &fleet)
        XCTAssertEqual(fleet.count, 1, "the same peripheral must fold into its own provisional row")
    }

    func testProvisionalRowsNeverCollideWithTheDemoFleet() {
        var fleet: [Witness] = []
        FleetMerge.attach([sighting()], to: &fleet)
        let row = FleetMerge.provisionalWitness(from:
            FleetSelfDevice(name: "N", online: true, chain: "unknown", product: "canary-display"),
            host: "canary-x.local")

        XCTAssertFalse(fleet[0].id.hasPrefix("demo-"))
        XCTAssertFalse(row.id.hasPrefix("demo-"))
        XCTAssertTrue(row.id.hasPrefix("lan:"))
    }
}
