// NearbyCanaryCardLogicTests.swift
//
// The nearby-Canary card and its setup sheet, where they decide rather than
// draw (NearbySetupLogic): the card outliving a sighting that went quiet,
// what the picker opens with and when the second Canary joins by itself,
// the way back from an empty list, which network sighting is THIS unit,
// and the done card's word on the hub — each a line here, so the view only
// follows.

import XCTest
@testable import SecuraCV

final class NearbyCanaryCardLogicTests: XCTestCase {

    private func canary(_ family: CanaryFamily, _ suffix: String, rssi: Int = -60, canScan: Bool = true) -> NearbyCanary {
        NearbyCanary(peripheralID: UUID(), family: family, suffix: suffix,
                     displayName: "\(family.rawValue.capitalized)-\(suffix)", rssiDBM: rssi, lastHeard: Date(),
                     improvState: .authorized, canIdentify: true, canScanWiFi: canScan)
    }

    private let home = HouseholdWiFi(ssid: "Home", password: "hunter22", savedAt: Date())
    private let heard = [ImprovWire.Network(ssid: "Home", rssi: -50, auth: "WPA2"),
                         ImprovWire.Network(ssid: "Next Door", rssi: -45, auth: "WPA2"),
                         ImprovWire.Network(ssid: "Cafe", rssi: -70, auth: "NO")]

    // MARK: - the card outlives the sighting

    func testTheCardKeepsTheCanaryBeingSetUpAfterItStopsAdvertising() {
        let a = canary(.sense, "K7MZ"), b = canary(.vision, "AB12")
        XCTAssertEqual(NearbySetupLogic.cardRows(nearby: [a, b], settingUp: nil), [a, b])
        XCTAssertEqual(NearbySetupLogic.cardRows(nearby: [a, b], settingUp: a), [a, b],
                       "still heard: listed once, where it was")
        // Connected, it stops advertising and leaves the nearby list
        // fifteen seconds in; the card — and the sheet hanging off it —
        // keep it until the sheet closes.
        XCTAssertEqual(NearbySetupLogic.cardRows(nearby: [b], settingUp: a), [b, a])
        XCTAssertEqual(NearbySetupLogic.cardRows(nearby: [], settingUp: a), [a], "alone on the card, not an empty card")
        XCTAssertEqual(NearbySetupLogic.cardRows(nearby: [], settingUp: nil), [], "sheet closed, nobody near: the card goes")
    }

    // MARK: - what the picker opens with

    func testNothingRememberedOpensUnselected() {
        // Never the strongest network the Canary heard — in a building
        // that is as often a neighbor's as yours.
        let o = NearbySetupLogic.opening(networks: heard, remembered: nil, canScan: true)
        XCTAssertEqual(o, NearbySetupLogic.Opening(chosenSSID: "", typedSSID: "", password: "", autoJoin: false))
        XCTAssertEqual(NearbySetupLogic.opening(networks: [], remembered: nil, canScan: true),
                       NearbySetupLogic.Opening(chosenSSID: "", typedSSID: "", password: "", autoJoin: false))
    }

    func testTheRememberedNetworkOnTheListJoinsByItself() {
        // The second Canary is two taps: Set up, Done.
        let o = NearbySetupLogic.opening(networks: heard, remembered: home, canScan: true)
        XCTAssertEqual(o, NearbySetupLogic.Opening(chosenSSID: "Home", typedSSID: "", password: "hunter22", autoJoin: true))
    }

    func testTheRememberedNetworkOffTheListIsNotGuessedAt() {
        // The Canary can hear networks and yours is not among them (a 5 GHz
        // name, another house): nothing picked, nothing prefilled, the hint
        // names it — never "the strongest" in its place.
        let elsewhere = [ImprovWire.Network(ssid: "Next Door", rssi: -45, auth: "WPA2")]
        XCTAssertEqual(NearbySetupLogic.opening(networks: elsewhere, remembered: home, canScan: true),
                       NearbySetupLogic.Opening(chosenSSID: "", typedSSID: "", password: "", autoJoin: false))
    }

    func testNoListTypesTheRememberedNameAndJoinsOnlyWhenTheCanaryCannotList() {
        // A Canary that cannot scan: the remembered name is all there is to
        // judge by, and it goes by itself.
        XCTAssertEqual(NearbySetupLogic.opening(networks: [], remembered: home, canScan: false),
                       NearbySetupLogic.Opening(chosenSSID: "", typedSSID: "Home", password: "hunter22", autoJoin: true))
        // A Canary that CAN scan but came back empty (pairing declined, or
        // the scan ran out of time): prefilled, but not sent unasked — the
        // list it could have given is the check that was skipped.
        XCTAssertEqual(NearbySetupLogic.opening(networks: [], remembered: home, canScan: true),
                       NearbySetupLogic.Opening(chosenSSID: "", typedSSID: "Home", password: "hunter22", autoJoin: false))
    }

    func testANameIsPickedFromTheListWhenItIsOnIt() {
        // What a router's QR, or a name typed before the list arrived, does
        // to the fields.
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "Home", listed: ["Home", "Cafe"]).chosen, "Home")
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "Home", listed: ["Home", "Cafe"]).typed, "")
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "Attic", listed: ["Home", "Cafe"]).chosen, "")
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "Attic", listed: ["Home", "Cafe"]).typed, "Attic")
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "Attic", listed: []).typed, "Attic", "no list: typed")
        XCTAssertEqual(NearbySetupLogic.pick(ssid: "", listed: ["Home"]).typed, "", "nothing read, nothing changed")
    }

    // MARK: - the way back from an empty list

    func testAnEmptyListNamesItsCauseAndTheWayBack() throws {
        let declined = try XCTUnwrap(NearbySetupLogic.emptyListHint(pairingDeclined: true, canScan: true))
        XCTAssertTrue(declined.contains("declined"), "the cause: the Pair sheet was waved away")
        XCTAssertTrue(declined.contains("List its networks again"), "the button on the sheet, by name")
        XCTAssertTrue(declined.contains("tap Pair"), "and what the retry will ask")
        let late = try XCTUnwrap(NearbySetupLogic.emptyListHint(pairingDeclined: false, canScan: true))
        XCTAssertTrue(late.contains("didn't arrive in time"), "the other cause: the scan ran out")
        XCTAssertTrue(late.contains("List its networks again"))
        XCTAssertFalse(late.contains("Pair"), "pairing already happened; no second ask")
        XCTAssertNil(NearbySetupLogic.emptyListHint(pairingDeclined: false, canScan: false),
                     "a Canary that cannot list has nothing to retry — the typed name is the way")
        XCTAssertNil(NearbySetupLogic.emptyListHint(pairingDeclined: true, canScan: false))
    }

    func testAJoinTheCanaryRefusedIsRetriedWhereItStands() {
        let v = NearbySetupLogic.retryVerdict(ssid: "Home")
        XCTAssertTrue(v.hasPrefix("The Canary couldn't join Home."), "the Canary's verdict, naming the network")
        XCTAssertTrue(v.contains("password") && v.contains("your network"), "Improv's one error names both causes")
        XCTAssertTrue(v.contains("tap Join again"), "same link, same fields — no reconnect")
    }

    // MARK: - is that THIS Canary?

    func testASenseOrVisionIsKnownByItsPseudonymNeverByKind() {
        XCTAssertTrue(NearbySetupLogic.isThisUnit(family: .vision, suffix: "K7MZ", knownDeviceID: nil,
                                                  seenType: .vision, seenDeviceID: "canary_vision_001",
                                                  seenHost: "canary-vision-001-k7mzq2.local"))
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .vision, suffix: "K7MZ", knownDeviceID: nil,
                                                   seenType: .vision, seenDeviceID: "canary_vision_001",
                                                   seenHost: "canary-vision-001-ab12cd.local"),
                       "a Vision you already own is not the one just set up")
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .vision, suffix: "K7MZ", knownDeviceID: nil,
                                                   seenType: .sense, seenDeviceID: "x",
                                                   seenHost: "canary-sense-001-k7mzq2.local"),
                       "the family must agree")
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .sense, suffix: "K7MZ", knownDeviceID: nil,
                                                   seenType: .sense, seenDeviceID: "x", seenHost: nil),
                       "a host-less advert proves nothing")
    }

    func testAWAPIsKnownByTheDeviceIDItsReceiptOrClaimNamed() {
        // Paired: the receipt's id.
        XCTAssertTrue(NearbySetupLogic.isThisUnit(family: .wap, suffix: "AB12", knownDeviceID: "wap-ab12",
                                                  seenType: .wap, seenDeviceID: "wap-ab12", seenHost: "canary-ab12.local"))
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .wap, suffix: "AB12", knownDeviceID: "wap-ab12",
                                                   seenType: .wap, seenDeviceID: "wap-ff00", seenHost: "canary-ff00.local"))
        // Joined but not paired: the claim named the unit too — it used to
        // be the one WAP the done card could never find.
        XCTAssertTrue(NearbySetupLogic.isThisUnit(family: .wap, suffix: "AB12", knownDeviceID: "wap-ab12",
                                                  seenType: .wap, seenDeviceID: "wap-ab12", seenHost: nil),
                      "the id is enough; a WAP's host carries no pseudonym")
        // Neither receipt nor claim: a WAP is never matched by its suffix.
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .wap, suffix: "AB12", knownDeviceID: nil,
                                                   seenType: .wap, seenDeviceID: "wap-ab12", seenHost: "canary-ab12.local"))
        // A known id wins over a pseudonym match for every family.
        XCTAssertFalse(NearbySetupLogic.isThisUnit(family: .vision, suffix: "K7MZ", knownDeviceID: "other",
                                                   seenType: .vision, seenDeviceID: "canary_vision_001",
                                                   seenHost: "canary-vision-001-k7mzq2.local"))
    }

    // MARK: - the hub

    func testTheDoneCardSaysWhatTheCanaryDidWithTheHub() {
        XCTAssertEqual(NearbySetupLogic.hubEnding(.answered(.stored, host: "homeassistant.local")),
                       "It reports to your hub at homeassistant.local.")
        XCTAssertEqual(NearbySetupLogic.hubEnding(.answered(.loginRefused, host: "homeassistant.local")),
                       "Your hub refused its login — open Set up → Your hub to re-mint the Canary login.")
        XCTAssertEqual(NearbySetupLogic.hubEnding(.noHub),
                       "No hub on this phone yet. Set one up from Set up; then tap this Canary's BOOT button once and it takes the hub from this phone.")
        XCTAssertEqual(NearbySetupLogic.hubEnding(.notOffered),
                       "Its hub link could not be handed over (an older firmware) — flash it once with the Flasher's MQTT fields.")
        // The endings the contract leaves to the Canary's byte: never a
        // claim the Canary did not make.
        XCTAssertTrue(NearbySetupLogic.hubEnding(.answered(.rejected, host: "h")).contains("didn't take"))
        XCTAssertTrue(NearbySetupLogic.hubEnding(.answered(.none, host: "h")).contains("kept no hub"))
        XCTAssertTrue(NearbySetupLogic.hubEnding(.answered(.unknown, host: "h")).contains("no answer in time"))
        XCTAssertTrue(NearbySetupLogic.hubEnding(.unsendable(host: "h")).contains("could not be handed over"))
        XCTAssertFalse(NearbySetupLogic.hubEnding(.answered(.unknown, host: "h")).contains("reports to your hub"),
                       "no answer is not a yes")
        // Only the stored answer is settled; everything else names a next step.
        XCTAssertTrue(NearbySetupLogic.HubHandoff.answered(.stored, host: "h").settled)
        for open in [NearbySetupLogic.HubHandoff.notOffered, .noHub, .unsendable(host: "h"),
                     .answered(.rejected, host: "h"), .answered(.loginRefused, host: "h"),
                     .answered(.none, host: "h"), .answered(.unknown, host: "h")] {
            XCTAssertFalse(open.settled, "\(open)")
        }
    }

    func testEverySentenceKeepsTheVoice() {
        // (US spellings and the fleet word are the repo-wide lints' job;
        // nothing here spells what they forbid.)
        for s in NearbySetupLogic.allCopy {
            XCTAssertFalse(s.isEmpty)
            XCTAssertTrue(s.hasSuffix("."), "a sentence: \(s)")
        }
        XCTAssertTrue(NearbySetupLogic.cameraDeniedNote.contains("Settings"), "the fix lives there; the row has the button")
        XCTAssertEqual(NearbySetupLogic.keepAliveSeconds, 60, "well inside the firmware's three-minute idle bound")
        XCTAssertTrue(NearbySetupLogic.keepAliveSeconds * 2 < 180)
        XCTAssertTrue((2...5).contains(NearbySetupLogic.autoJoinCountdown), "long enough to tap Change, short enough to be two taps")
    }
}
