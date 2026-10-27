// SetupGuideTests.swift
//
// The walkthroughs are data; these pin their shape. Every family has a
// path and the one action that carries it out, the hub walkthrough acts
// in the order the hub actually demands (watch → account → finish), and
// no step is an empty promise.

import XCTest
@testable import SecuraCV

final class SetupGuideTests: XCTestCase {

    func testTheHubWalkthroughActsInTheOrderTheHubDemands() {
        let actions = SetupGuide.hub.compactMap(\.action)
        XCTAssertEqual(actions, [.watchHub, .ownerAccount, .provisionHub])
        XCTAssertEqual(SetupGuide.hub.first?.id, "need")
        XCTAssertEqual(SetupGuide.hub.last?.id, "next")
    }

    func testEveryFamilyHasAPathAndTheActionThatDrivesIt() {
        for family in CanaryFamily.allCases {
            let steps = SetupGuide.canary(family)
            let actions = steps.compactMap(\.action)
            let door: [SetupAction] = family.hasBluetoothDoor ? [.nearbyCanary] : []
            switch family.path {
            case .setupNetwork:
                XCTAssertEqual(actions, door + [.readSetupKey, .joinSetupNetwork, .watchForCanary], family.rawValue)
            case .ownPage:
                // Its own page speaks its own wizard: explained, driven by
                // the person — no control that pretends to drive it.
                XCTAssertEqual(actions, door + [.watchForCanary], family.rawValue)
            }
            XCTAssertEqual(steps.first?.title, family.title)
            XCTAssertFalse(family.tagline.isEmpty)
            XCTAssertFalse(family.hubNote.isEmpty)
        }
    }

    func testTheWAPFallsBackToItsOwnPageAndOnlyDisplaysShowAQR() {
        // The WAP's old fallback, "its bonded Bluetooth service", could
        // never list a device: a new WAP does not advertise that console,
        // and its reads need a PIN confirmed on the device. Its real
        // fallback is its own setup page.
        XCTAssertEqual(CanaryFamily.wap.path, .ownPage)
        let page = SetupGuide.canary(.wap).first { $0.id == "page" }
        XCTAssertNotNil(page)
        XCTAssertTrue(page?.title.hasPrefix("If no card appears") ?? false)
        XCTAssertFalse(SetupGuide.canary(.wap).contains { $0.body.contains("bonded Bluetooth") },
                       "no step promises the bonded path")
        // canary_wap.ino generate_ap_ssid(): SecuraCV-wap-<4>, the node type
        // between the prefix and the suffix (onboarding_fsm.h).
        XCTAssertTrue(page?.body.contains("SecuraCV-wap-XXXX") ?? false,
                      "the page step names the WAP's setup network as it is broadcast")
        XCTAssertEqual(CanaryFamily.display.path, .setupNetwork(keySource: .glassQR))
        XCTAssertEqual(CanaryFamily.vision.path, .setupNetwork(keySource: .flasher))
        XCTAssertEqual(CanaryFamily.sense.path, .setupNetwork(keySource: .flasher))
    }

    func testTheBluetoothDoorIsOnTheHeadlessWitnessesAndLeadsTheirWalkthrough() {
        // The families whose firmware compiles common/network/improv_ble —
        // nothing here promises a door a device does not open.
        XCTAssertTrue(CanaryFamily.sense.hasBluetoothDoor)
        XCTAssertTrue(CanaryFamily.vision.hasBluetoothDoor)
        XCTAssertTrue(CanaryFamily.wap.hasBluetoothDoor, "the WAP's door hands over its pairing key too")
        XCTAssertFalse(CanaryFamily.display.hasBluetoothDoor, "a display shows a QR on its glass instead")
        for family in CanaryFamily.allCases where family.hasBluetoothDoor {
            let steps = SetupGuide.canary(family)
            XCTAssertEqual(steps[1].action, .nearbyCanary, family.rawValue)
            XCTAssertEqual(steps[1].id, "nearby")
            XCTAssertTrue(steps[2].title.hasPrefix("If no card appears"), "\(family.rawValue): the older path is the fallback")
        }
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("pairing key"), "the WAP step says what the one tap earns")
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("never rides Bluetooth"),
                      "and where the key crosses: a claim over the link, the key over Wi-Fi")
        XCTAssertFalse(SetupGuide.canary(.sense)[1].body.contains("pairing key"), "a Sense has no key to hand over")
        // The door's rules, as the firmware keeps them: the first-boot
        // window on every door family; the BOOT tap only where a board
        // reads one (Sense, Vision) — a WAP has no tap door yet, so its
        // step says to power-cycle it instead.
        for family in CanaryFamily.allCases where family.hasBluetoothDoor {
            let body = SetupGuide.canary(family)[1].body
            XCTAssertTrue(body.contains(SetupGuide.doorWindowSentence),
                          "\(family.rawValue): the window is the door's first rule")
            XCTAssertEqual(body.contains(SetupGuide.tapSentence), family != .wap,
                           "\(family.rawValue): only a Sense or Vision reads a BOOT tap for the door")
        }
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("power-cycle"), "a WAP's door re-arms on a power cycle")
        XCTAssertFalse(SetupGuide.canary(.display)[1].body.contains(SetupGuide.doorWindowSentence),
                       "a display has no door to keep open")
        // A Sense or Vision has both ways back in, and the power cycle is
        // said first: it is the one that works past the half hour too.
        for family in [CanaryFamily.sense, .vision] {
            let line = SetupGuide.doorWindowLine(family)
            XCTAssertTrue(line.contains("unplug and replug it for another half hour"), family.rawValue)
            XCTAssertTrue(line.contains(SetupGuide.tapSentence), family.rawValue)
        }
    }

    func testNothingYetSaysBothWaysBackIn() {
        // The footnote every "Listening…" grows after the wait: the two
        // reasons a door is shut, and both ways to reopen it.
        let line = SetupGuide.nothingYetLine
        XCTAssertTrue(line.hasPrefix("Nothing yet?"))
        XCTAssertTrue(line.contains("more than half an hour"), "the first-boot window")
        XCTAssertTrue(line.contains("was on a Wi-Fi before"), "a saved network shuts the door for good")
        XCTAssertTrue(line.contains("unplug it and plug it back in"))
        XCTAssertTrue(line.contains("BOOT button"), "the Sense's and Vision's shorter way")
        XCTAssertTrue(line.hasSuffix("."))
        XCTAssertEqual(SetupGuide.nothingYetAfter, 20, "long enough for a door to be heard")
    }

    func testTheHeadlessFallbackLeadsWithTheReplugAndNamesTheConsole() {
        for family in [CanaryFamily.sense, .vision] {
            let key = SetupGuide.canary(family).first { $0.id == "key" }!
            XCTAssertEqual(key.title, "If no card appears", family.rawValue)
            XCTAssertTrue(key.body.hasPrefix("Unplug it and plug it back in"), "\(family.rawValue): the replug comes first")
            XCTAssertTrue(key.body.contains("BOOT button"), family.rawValue)
            // The Flasher prints nothing today: the key is on the device's
            // own serial line.
            XCTAssertTrue(key.body.contains("Flasher's serial console (the [SETUP] line at boot)"), family.rawValue)
            XCTAssertTrue(key.short.contains("Unplug and replug"), family.rawValue)
        }
        for step in SetupGuide.allSteps {
            XCTAssertFalse(step.title.contains("Flasher printed"), step.id)
            XCTAssertFalse(step.body.contains("Flasher printed"), step.id)
            XCTAssertFalse(step.short.contains("Flasher printed"), step.id)
        }
    }

    func testTheHeadlessHubNoteNamesTheSameTapAndTheBootFallback() {
        for family in [CanaryFamily.sense, .vision] {
            let note = family.hubNote
            XCTAssertTrue(note.hasPrefix("The same tap that gives it your Wi-Fi hands it your hub"), family.rawValue)
            XCTAssertTrue(note.contains("broker address"), family.rawValue)
            XCTAssertTrue(note.contains("Set the hub up first"), family.rawValue)
            XCTAssertTrue(note.contains("BOOT button once after the hub exists"), family.rawValue)
            XCTAssertFalse(note.contains("Flasher"), "\(family.rawValue): no MQTT fields to type")
        }
        // Nothing in either walkthrough still sends people to a terminal for
        // the integration: the kernel add-on's panel installs it with one click.
        for step in SetupGuide.allSteps {
            XCTAssertFalse(step.body.contains("terminal"), step.id)
        }
        XCTAssertTrue(SetupGuide.hub.first { $0.id == "finish" }!.body.contains("SecuraCV panel in the hub's sidebar"),
                      "the finish step names the panel that installs the integration")
    }

    func testEveryFamilyHasOneWordAndTheDoorFamiliesPutItOnTheAir() {
        XCTAssertEqual(CanaryFamily.sense.word, "Sense")
        XCTAssertEqual(CanaryFamily.vision.word, "Vision")
        XCTAssertEqual(CanaryFamily.wap.word, "WAP")
        XCTAssertEqual(CanaryFamily.display.word, "Display")
        for family in CanaryFamily.allCases where family.hasBluetoothDoor {
            XCTAssertEqual(NearbyCanaries.parseName("\(family.word)-AB12")?.family, family,
                           "\(family.rawValue): the word is the one the advert spells")
        }
    }

    func testNoStepIsAnEmptyPromise() {
        for step in SetupGuide.allSteps {
            XCTAssertFalse(step.title.isEmpty, step.id)
            XCTAssertGreaterThan(step.body.count, 40, "\(step.id) says too little")
        }
        let ids = SetupGuide.hub.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
    }

    func testEveryStepSaysItselfInOneShortSentence() {
        // The screen shows `short` first and the paragraph behind "How it
        // works" — a step that needs more than twenty words up front is a
        // manual, not a card. The long body stays, honest and pinned above.
        for step in SetupGuide.allSteps {
            let words = step.short.split(separator: " ").count
            XCTAssertGreaterThan(words, 2, "\(step.id): says something")
            XCTAssertLessThanOrEqual(words, 20, "\(step.id): \(step.short)")
            XCTAssertLessThan(step.short.count, step.body.count, "\(step.id): the short line is shorter")
        }
    }

    func testFamiliesMapToTheCoarseTypesTheFleetUses() {
        XCTAssertEqual(CanaryFamily.display.deviceType, .display)
        XCTAssertEqual(CanaryFamily.vision.deviceType, .vision)
        XCTAssertEqual(CanaryFamily.sense.deviceType, .sense)
        XCTAssertEqual(CanaryFamily.wap.deviceType, .wap)
    }
}
