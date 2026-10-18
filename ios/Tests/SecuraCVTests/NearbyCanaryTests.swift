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
        // A Sense or Vision names itself from its pseudonym, which is drawn
        // from the 54-character no-confusion alphabet and upper-cased — not
        // hex. The hex-only rule turned real Canaries into family-less rows.
        XCTAssertEqual(NearbyCanaries.parseName("Sense-K7MZ")?.family, .sense)
        XCTAssertEqual(NearbyCanaries.parseName("Sense-K7MZ")?.suffix, "K7MZ")
        XCTAssertEqual(NearbyCanaries.parseName("Vision-RT9W")?.family, .vision)
        XCTAssertNil(NearbyCanaries.parseName("Sense-K7M!"), "only letters and digits")
        XCTAssertNil(NearbyCanaries.parseName("Sense-AB1"))
        XCTAssertNil(NearbyCanaries.parseName("Dash-AB12"), "displays do not open a Bluetooth door")
        XCTAssertNil(NearbyCanaries.parseName("Sense"))
        XCTAssertNil(NearbyCanaries.parseName(""))
    }

    func testTheTitleNamesTheFamilyAndReadsBack() {
        XCTAssertEqual(NearbyCanaries.title(family: .sense, suffix: "K7MZ"), "Canary Sense K7MZ")
        XCTAssertEqual(NearbyCanaries.title(family: .vision, suffix: "K7MZ"), "Canary Vision K7MZ")
        XCTAssertEqual(NearbyCanaries.title(family: .wap, suffix: "AB12"), "Canary WAP AB12")
        // The Fleet row keeps this title after the door shuts, and the dedupe
        // against the same unit's mDNS row has to recover the suffix from it.
        XCTAssertEqual(NearbyCanaries.parseTitle("Canary Sense K7MZ")?.family, .sense)
        XCTAssertEqual(NearbyCanaries.parseTitle("Canary Sense K7MZ")?.suffix, "K7MZ")
        XCTAssertEqual(NearbyCanaries.parseTitle("Canary WAP AB12")?.family, .wap)
        XCTAssertNil(NearbyCanaries.parseTitle("Canary Display AB12"), "a display opens no door")
        XCTAssertNil(NearbyCanaries.parseTitle("Canary Sense ab12"), "as strict as the advert grammar")
        XCTAssertNil(NearbyCanaries.parseTitle("Front Door"), "a person's name for a Canary is not a door")
        XCTAssertNil(NearbyCanaries.parseTitle("SCV-AB12"))
        for (family, suffix) in [(CanaryFamily.sense, "K7MZ"), (.vision, "0F9C"), (.wap, "AB12")] {
            let back = NearbyCanaries.parseTitle(NearbyCanaries.title(family: family, suffix: suffix))
            XCTAssertEqual(back?.family, family)
            XCTAssertEqual(back?.suffix, suffix)
        }
        XCTAssertEqual(NearbyCanaries.parseDoorName("Sense-K7MZ")?.suffix, "K7MZ")
        XCTAssertEqual(NearbyCanaries.parseDoorName("Canary Sense K7MZ")?.suffix, "K7MZ")
        XCTAssertNil(NearbyCanaries.parseDoorName("SCV-K7MZ"))
    }

    // MARK: - the name outlives the door

    func testTheRowKeepsTheDoorsTitleAfterTheNameLeavesTheAir() {
        let id = UUID()
        var names: [UUID: String] = [:]
        // While the door is open the scan response carries "Sense-K7MZ".
        FleetMerge.rememberDoorNames([heard(id, name: "Sense-K7MZ")], into: &names)
        XCTAssertEqual(names[id], "Canary Sense K7MZ")
        // A stray local name is never a title.
        let other = UUID()
        FleetMerge.rememberDoorNames([heard(other, name: "Kitchen speaker")], into: &names)
        XCTAssertNil(names[other])
        // The door shut: the beacon alone is on the air, named by its
        // fingerprint suffix — a different four characters. The remembered
        // title stands in; a peripheral no door named keeps "SCV-XXXX".
        let b = beacon(flags: 0, fp: (0x3F, 0x1A))
        let shut = BeaconSighting(beacon: b, rssiDBM: -60, lastHeard: now, peripheralID: id, localName: nil)
        let stranger = BeaconSighting(beacon: b, rssiDBM: -60, lastHeard: now, peripheralID: other, localName: nil)
        let named = FleetMerge.naming([shut, stranger], remembered: names)
        XCTAssertEqual(named[0].displayName, "Canary Sense K7MZ")
        XCTAssertEqual(named[1].displayName, "SCV-3F1A")
        XCTAssertEqual(FleetMerge.provisionalWitness(from: named[0]).name, "Canary Sense K7MZ")
        // During the door, the raw advert name is replaced by the same title,
        // so the row reads the same before and after.
        let open = BeaconSighting(beacon: b, rssiDBM: -60, lastHeard: now, peripheralID: id, localName: "Sense-K7MZ")
        XCTAssertEqual(FleetMerge.naming([open], remembered: names).first?.displayName, "Canary Sense K7MZ")
        // And the dedupe against the unit's mDNS row still finds the suffix in the title.
        var row = Witness(id: "ble:" + id.uuidString)
        row.name = "Canary Sense K7MZ"
        let joined = DiscoveredCanary(id: "canary-sense-001-k7mzq2", deviceID: "canary_sense_001", name: "Sense",
                                      deviceType: .sense, publishedType: "canary-sense", hardware: "",
                                      host: "canary-sense-001-k7mzq2", firmware: "1.0", model: "")
        let rows = FleetMerge.discoveredRows(found: [joined], fleet: [row], pairedIDs: [])
        XCTAssertTrue(rows.onNetwork.isEmpty, "one Canary, one row, right after it joins")
    }

    func testAnOpenDoorWithTheImprovServiceIsACandidate() {
        let c = NearbyCanaries.candidates(from: [heard()], pairedFingerprints: [], dismissedSuffixes: [], now: now)
        XCTAssertEqual(c.count, 1)
        XCTAssertEqual(c.first?.family, .sense)
        XCTAssertEqual(c.first?.suffix, "AB12")
        XCTAssertEqual(c.first?.title, "Canary Sense AB12", "the family word, not the job title")
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

    // MARK: - the card that comes to you

    private func candidate(_ suffix: String, rssi: Int, family: CanaryFamily? = .sense) -> NearbyCanary {
        NearbyCanary(peripheralID: UUID(), family: family, suffix: suffix, displayName: "Sense-\(suffix)",
                     rssiDBM: rssi, lastHeard: now, improvState: .authorized, canIdentify: true, canScanWiFi: true)
    }

    func testAFirstOwnersOneCanaryIsOfferedFromAcrossTheRoom() {
        // A WAP powered on in its final spot: "in the room", not "close by".
        let acrossTheRoom = candidate("K7MZ", rssi: -80)
        XCTAssertNil(NearbyCanaries.autoOffer([acrossTheRoom], alreadyOffered: [], hasRealFleet: true),
                     "with a fleet already, the nearer gate protects against a neighbor's new Canary")
        XCTAssertEqual(NearbyCanaries.autoOffer([acrossTheRoom], alreadyOffered: [], hasRealFleet: false), acrossTheRoom,
                       "no fleet yet and one Canary in earshot: there is no ambiguity to protect")
        XCTAssertNil(NearbyCanaries.autoOffer([candidate("K7MZ", rssi: -86)], alreadyOffered: [], hasRealFleet: false),
                     "faint is still faint")
        XCTAssertNil(NearbyCanaries.autoOffer([acrossTheRoom, candidate("Q2WX", rssi: -82)], alreadyOffered: [],
                                              hasRealFleet: false),
                     "two in earshot is a choice even for a first owner")
        XCTAssertEqual(NearbyCanaries.offerThreshold(candidateCount: 1, hasRealFleet: false), NearbyCanaries.inTheRoomDBM)
        XCTAssertEqual(NearbyCanaries.offerThreshold(candidateCount: 1, hasRealFleet: true), NearbyCanaries.closeEnoughDBM)
        XCTAssertEqual(NearbyCanaries.offerThreshold(candidateCount: 2, hasRealFleet: false), NearbyCanaries.closeEnoughDBM)
        XCTAssertNil(NearbyCanaries.autoOffer([acrossTheRoom], alreadyOffered: []), "the default is the careful gate")
    }

    // MARK: - saying it again, after a replug

    func testAnOfferLapsesOnceTheDoorLeavesTheAirAndReturns() {
        var ledger = NearbyOfferLedger()
        ledger.noteOffered("K7MZ", now: now)
        XCTAssertTrue(ledger.suppressed.contains("K7MZ"))
        // Still on the air: offered stays offered, however long.
        ledger.observe(onAir: ["K7MZ"], now: now.addingTimeInterval(600))
        XCTAssertTrue(ledger.suppressed.contains("K7MZ"))
        // Off the air for less than the freshness window: a wobble, still suppressed.
        ledger.observe(onAir: [], now: now.addingTimeInterval(600 + NearbyCanaries.freshFor - 1))
        XCTAssertTrue(ledger.suppressed.contains("K7MZ"))
        // Past the window: a replug is a new first boot — forgotten.
        ledger.observe(onAir: [], now: now.addingTimeInterval(600 + NearbyCanaries.freshFor + 1))
        XCTAssertFalse(ledger.suppressed.contains("K7MZ"), "the card may come once more")
        ledger.observe(onAir: ["K7MZ"], now: now.addingTimeInterval(700))
        XCTAssertFalse(ledger.suppressed.contains("K7MZ"))
    }

    func testNotNowHoldsUntilTheSameGap() {
        var ledger = NearbyOfferLedger()
        ledger.noteDismissed("AB12", now: now)
        XCTAssertTrue(ledger.dismissed.contains("AB12"))
        // A quick replug (off the air briefly) does not undo "Not now".
        ledger.observe(onAir: [], now: now.addingTimeInterval(5))
        ledger.observe(onAir: ["AB12"], now: now.addingTimeInterval(9))
        XCTAssertTrue(ledger.dismissed.contains("AB12"), "the gap was shorter than the window")
        // Gone for longer, then back: the person may be asked again.
        ledger.observe(onAir: [], now: now.addingTimeInterval(9 + NearbyCanaries.freshFor + 1))
        XCTAssertFalse(ledger.dismissed.contains("AB12"))
        // Other suffixes are untouched throughout.
        ledger.noteDismissed("ZZZZ", now: now.addingTimeInterval(30))
        ledger.observe(onAir: ["ZZZZ"], now: now.addingTimeInterval(60))
        XCTAssertEqual(ledger.suppressed, ["ZZZZ"])
    }

    // MARK: - the one notification from the background

    func testTheBackgroundNoticeIsOncePerCanaryAndNamesIt() {
        let one = candidate("K7MZ", rssi: -70)
        XCTAssertEqual(NearbyCanaries.backgroundNotice(for: [one], alreadyNoticed: []), one)
        XCTAssertNil(NearbyCanaries.backgroundNotice(for: [one], alreadyNoticed: ["K7MZ"]), "told once")
        let two = candidate("Q2WX", rssi: -80)
        XCTAssertEqual(NearbyCanaries.backgroundNotice(for: [one, two], alreadyNoticed: ["K7MZ"]), two)
        XCTAssertNil(NearbyCanaries.backgroundNotice(for: [], alreadyNoticed: []))
        XCTAssertEqual(NearbyCanaries.noticeTitle, "A new Canary is nearby")
        XCTAssertEqual(NearbyCanaries.noticeBody(for: one), "Canary Sense K7MZ is ready for your Wi-Fi. Tap to set it up.")
        XCTAssertFalse(NearbyCanaries.noticeThreadID.isEmpty, "the tap routes by thread")
    }

    func testOneCloseCanaryIsOfferedByItself() {
        let one = candidate("K7MZ", rssi: -50)
        XCTAssertEqual(NearbyCanaries.autoOffer([one], alreadyOffered: []), one)
        XCTAssertEqual(NearbyCanaries.autoOffer([one, candidate("FAR1", rssi: -88)], alreadyOffered: []), one,
                       "a faint one in the next room does not make the close one a choice")
    }

    func testTwoCloseCanariesAreAChoiceNotAGuess() {
        XCTAssertNil(NearbyCanaries.autoOffer([candidate("AAAA", rssi: -50), candidate("BBBB", rssi: -60)],
                                              alreadyOffered: []),
                     "two on the table belong to the list, where each row has its own Blink")
    }

    func testAnOfferedCanaryIsNotOfferedAgain() {
        let one = candidate("K7MZ", rssi: -50)
        XCTAssertNil(NearbyCanaries.autoOffer([one], alreadyOffered: ["K7MZ"]))
        // With the first one answered, the second on the table may come up.
        let two = candidate("Q2WX", rssi: -55)
        XCTAssertEqual(NearbyCanaries.autoOffer([one, two], alreadyOffered: ["K7MZ"]), two)
    }

    func testAFaintCanaryIsNotOfferedByItself() {
        XCTAssertNil(NearbyCanaries.autoOffer([candidate("K7MZ", rssi: -71)], alreadyOffered: []))
        XCTAssertNotNil(NearbyCanaries.autoOffer([candidate("K7MZ", rssi: NearbyCanaries.closeEnoughDBM)],
                                                 alreadyOffered: []), "the band edge counts as close")
    }

    // MARK: - the card's latch (NearbyOfferOverlay draws NearbyOfferLatch)

    func testTheCardLatchesOnceAndCountsItOfferedThen() {
        let one = candidate("K7MZ", rssi: -50)
        var card = NearbyOfferLatch()
        XCTAssertEqual(card.latch(one, setupOpen: false, standingDown: false), "K7MZ",
                       "the suffix to count as offered, the moment the card appears")
        XCTAssertEqual(card.visible(standingDown: false), one)
        XCTAssertNil(card.latch(candidate("Q2WX", rssi: -50), setupOpen: false, standingDown: false),
                     "a latched card holds its Canary; a second offer waits")
        XCTAssertEqual(card.latched, one)
        XCTAssertNil(card.latch(nil, setupOpen: false, standingDown: false))
    }

    func testNothingLatchesWhileASetupSheetIsOpen() {
        var card = NearbyOfferLatch()
        XCTAssertNil(card.latch(candidate("K7MZ", rssi: -50), setupOpen: true, standingDown: false))
        XCTAssertNil(card.visible(standingDown: false))
    }

    func testTheCardStandsDownWhileThePlusSheetIsUp() {
        // The "+" sheet lists the same Canaries inline; at its .medium detent
        // the card would show dimmed behind it (sweep A75).
        let one = candidate("K7MZ", rssi: -50)
        var card = NearbyOfferLatch()
        XCTAssertNil(card.latch(one, setupOpen: false, standingDown: true),
                     "nothing latches behind the sheet, so nothing is counted as offered there")
        XCTAssertNil(card.visible(standingDown: true))
        XCTAssertNil(card.latched)
        // The sheet closed: the same offer latches now.
        XCTAssertEqual(card.latch(one, setupOpen: false, standingDown: false), "K7MZ")
        XCTAssertEqual(card.visible(standingDown: false), one)
    }

    func testACardAlreadyUpHidesBehindThePlusSheetAndComesBack() {
        let one = candidate("K7MZ", rssi: -50)
        var card = NearbyOfferLatch()
        _ = card.latch(one, setupOpen: false, standingDown: false)
        XCTAssertNil(card.visible(standingDown: true), "not drawn while the sheet is up")
        XCTAssertEqual(card.latched, one, "but still held, unanswered")
        XCTAssertEqual(card.visible(standingDown: false), one, "and back when the sheet goes")
    }

    func testAnAnswerOrLeavingEarshotLetsGo() {
        let one = candidate("K7MZ", rssi: -50)
        var card = NearbyOfferLatch()
        _ = card.latch(one, setupOpen: false, standingDown: false)
        card.heard(["K7MZ", "Q2WX"])
        XCTAssertEqual(card.latched, one, "still heard: still held")
        card.heard(["Q2WX"])
        XCTAssertNil(card.latched, "out of earshot entirely: let go")
        _ = card.latch(one, setupOpen: false, standingDown: false)
        card.answer()
        XCTAssertNil(card.visible(standingDown: false), "Continue or Not now: let go")
    }

    // MARK: - is that THIS Canary?

    func testTheJoinedDeviceIsMatchedByItsPseudonymNotItsKind() {
        // The firmware builds the advert name and the mDNS host from one
        // pseudonym: "Vision-K7MZ" and "canary-vision-001-k7MzQ2".
        XCTAssertTrue(NearbyCanaries.isSameDevice(suffix: "K7MZ", host: "canary-vision-001-k7MzQ2"))
        XCTAssertTrue(NearbyCanaries.isSameDevice(suffix: "K7MZ", host: "canary-vision-001-k7mzq2.local"))
        XCTAssertFalse(NearbyCanaries.isSameDevice(suffix: "K7MZ", host: "canary-vision-001-ffee00"),
                       "the Vision you already own is not the new one")
        XCTAssertFalse(NearbyCanaries.isSameDevice(suffix: "K7MZ", host: nil), "no host, no claim")
        XCTAssertFalse(NearbyCanaries.isSameDevice(suffix: "K7MZ", host: ""))
        XCTAssertFalse(NearbyCanaries.isSameDevice(suffix: "K7", host: "canary-vision-001-k7mzq2"),
                       "a suffix that is not four characters proves nothing")
    }

    func testTheSetupNetworkNamesTheSameFourCharacters() {
        XCTAssertEqual(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-aB3k"), "aB3k")
        XCTAssertEqual(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-aB3k-x9"), "aB3k",
                       "the per-session tag after a non-durable key is not identity")
        // The typed names (onboarding_fsm.h ap_ssid): the node type in front
        // is not identity either — the same four characters follow it.
        XCTAssertEqual(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-sense-aB3k"), "aB3k")
        XCTAssertEqual(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-sentinel-aB3k-x9"), "aB3k")
        XCTAssertEqual(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-display-CNRY"), "CNRY", "the fallback suffix")
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-Sense-aB3k"), "a type is lowercase")
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-s3nse-aB3k"), "a type has no digits")
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-sense-aB3k-x9z"), "a tag is two characters")
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-a-b-c-d"))
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "HomeWiFi"))
        XCTAssertNil(NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-ab"))
        // And the two halves meet: the setup network's characters match the
        // host the same unit later announces.
        let s = NearbyCanaries.suffix(ofSetupSSID: "SecuraCV-aB3k")!
        XCTAssertTrue(NearbyCanaries.isSameDevice(suffix: s, host: "canary-dash-001-aB3kZx"))
    }
}
